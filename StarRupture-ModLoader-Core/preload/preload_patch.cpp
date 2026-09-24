#include "preload/preload_patch.h"
#include "hooks/hooks_common.h"
#include "hooks/hook_broker.h"
#include "memory_scanner/image_info.h"
#include "core/version_check.h"
#include "logging/log.h"

#include <mutex>
#include <string>
#include <vector>

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

namespace
{
	struct OwnedHook
	{
		const IPluginSelf* owner;
		std::string        name;
		Hooks::Hook        hook;
	};

	std::mutex             g_mutex;
	std::vector<OwnedHook> g_hooks;

	// Cached because the interface hands out a const char* that the plugin may
	// keep, and a std::string built per call would dangle the moment it
	// returned.
	std::string g_gameVersionUtf8;

	// True when [address, address+size) is inside some loaded module. A preload
	// plugin runs before the game has started, so a bad offset here is a write
	// into whatever happens to be mapped -- worth one GetModuleHandleEx to turn
	// that into a refusal.
	bool IsInLoadedModule(uintptr_t address, size_t size)
	{
		if (!address || !size)
			return false;

		HMODULE module = nullptr;
		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(address), &module) || !module)
			return false;

		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		if (!image.valid)
			return false;

		return address >= image.base && (address + size) <= (image.base + image.size);
	}

	const char* OwnerName(const IPluginSelf* self)
	{
		return (self && self->name) ? self->name : "(unknown preload plugin)";
	}

	// --- IPreloadPatch entry points -----------------------------------------

	HMODULE   PatchGetGameModule()     { return GetModuleHandleW(nullptr); }
	uintptr_t PatchGetGameModuleBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

	size_t PatchGetGameModuleSize()
	{
		const Scanner::ImageInfo image = Scanner::GetImageInfo(GetModuleHandleW(nullptr));
		return image.valid ? image.size : 0;
	}

	const char* PatchGetGameVersion()
	{
		static bool s_resolved = false;
		if (!s_resolved)
		{
			s_resolved = true;
			const std::wstring wide = GetGameVersionString();
			if (!wide.empty())
			{
				char buffer[128]{};
				WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buffer,
					static_cast<int>(sizeof(buffer)), nullptr, nullptr);
				g_gameVersionUtf8 = buffer;
			}
		}
		return g_gameVersionUtf8.c_str();
	}

	const char* PatchGetLoaderBuildTag() { return MODLOADER_BUILD_TAG; }

	bool PatchReadBytes(uintptr_t address, void* dest, size_t size)
	{
		if (!dest || !IsInLoadedModule(address, size))
			return false;

		memcpy(dest, reinterpret_cast<const void*>(address), size);
		return true;
	}

	bool PatchWriteBytes(uintptr_t address, const void* source, size_t size)
	{
		if (!source || !IsInLoadedModule(address, size))
		{
			LogToFile::Error("[Preload] WriteBytes refused: 0x%llX+%zu is not inside a loaded module",
				static_cast<unsigned long long>(address), size);
			return false;
		}

		return Hooks::Patch(address, static_cast<const uint8_t*>(source), size);
	}

	bool PatchNop(uintptr_t address, size_t size)
	{
		if (!IsInLoadedModule(address, size))
		{
			LogToFile::Error("[Preload] Nop refused: 0x%llX+%zu is not inside a loaded module",
				static_cast<unsigned long long>(address), size);
			return false;
		}

		return Hooks::Nop(address, size);
	}

	bool PatchInstallHook(const IPluginSelf* self, const char* name, uintptr_t target,
		void* detour, void** outOriginal)
	{
		if (!name || !name[0])
			name = "(unnamed)";

		// A target of 0 is a pattern that did not resolve being passed straight
		// through. That cannot happen for anything resolved in PreloadScan --
		// the plugin would have been refused -- but it can for an address a
		// plugin worked out for itself, and patching address 0 before the game
		// has started is a crash with no stack worth reading.
		if (!target || !detour)
		{
			LogToFile::Error("[Preload] %s: InstallHook('%s') refused -- %s is null",
				OwnerName(self), name, target ? "detour" : "target");
			return false;
		}

		if (!IsInLoadedModule(target, 1))
		{
			LogToFile::Error("[Preload] %s: InstallHook('%s') refused -- target 0x%llX is not inside a loaded module",
				OwnerName(self), name, static_cast<unsigned long long>(target));
			return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);

		// One name per owner, so RemoveHook has something unambiguous to match.
		// Note what is NOT refused here: another plugin, or the loader itself,
		// already hooking this same address. Hooks::Broker turns that into a
		// chain -- both detours run, in install order -- instead of the silent
		// corruption two detours on one address used to produce.
		for (const OwnedHook& existing : g_hooks)
		{
			if (existing.owner == self && existing.name == name)
			{
				LogToFile::Error("[Preload] %s: InstallHook('%s') refused -- already installed under that name",
					OwnerName(self), name);
				return false;
			}
		}

		const int existingLinks = Hooks::Broker::GetLinkCount(target);

		g_hooks.push_back(OwnedHook{ self, name, Hooks::Hook{} });
		OwnedHook& entry = g_hooks.back();

		if (!entry.hook.Install(target, detour, outOriginal, OwnerName(self), name))
		{
			g_hooks.pop_back();
			LogToFile::Error("[Preload] %s: InstallHook('%s') FAILED at 0x%llX",
				OwnerName(self), name, static_cast<unsigned long long>(target));
			return false;
		}

		if (existingLinks > 0)
		{
			LogToFile::Info("[Preload] %s: hooked '%s' at 0x%llX -- joining %d existing hook(s) on that address",
				OwnerName(self), name, static_cast<unsigned long long>(target), existingLinks);
		}
		else
		{
			LogToFile::Info("[Preload] %s: hooked '%s' at 0x%llX",
				OwnerName(self), name, static_cast<unsigned long long>(target));
		}
		return true;
	}

	bool PatchRemoveHook(const IPluginSelf* self, const char* name)
	{
		if (!name) return false;

		std::lock_guard<std::mutex> lock(g_mutex);

		for (size_t i = 0; i < g_hooks.size(); ++i)
		{
			if (g_hooks[i].owner != self || g_hooks[i].name != name)
				continue;

			g_hooks[i].hook.Remove();
			g_hooks.erase(g_hooks.begin() + static_cast<ptrdiff_t>(i));
			LogToFile::Info("[Preload] %s: removed hook '%s'", OwnerName(self), name);
			return true;
		}

		return false;
	}

	IPreloadPatch g_table = {
		PatchGetGameModule,
		PatchGetGameModuleBase,
		PatchGetGameModuleSize,
		PatchGetGameVersion,
		PatchGetLoaderBuildTag,
		PatchReadBytes,
		PatchWriteBytes,
		PatchNop,
		PatchInstallHook,
		PatchRemoveHook
	};
}

namespace PreloadPatch
{
	IPreloadPatch* GetTable()
	{
		return &g_table;
	}

	int RemoveAllHooksFor(const IPluginSelf* owner)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		int removed = 0;
		for (size_t i = g_hooks.size(); i > 0; --i)
		{
			OwnedHook& entry = g_hooks[i - 1];
			if (entry.owner != owner)
				continue;

			entry.hook.Remove();
			g_hooks.erase(g_hooks.begin() + static_cast<ptrdiff_t>(i - 1));
			++removed;
		}

		return removed;
	}

	int CountHooksFor(const IPluginSelf* owner)
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		int count = 0;
		for (const OwnedHook& entry : g_hooks)
			if (entry.owner == owner)
				++count;
		return count;
	}

	void RemoveAllHooks()
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		for (size_t i = g_hooks.size(); i > 0; --i)
			g_hooks[i - 1].hook.Remove();

		g_hooks.clear();
	}
}
