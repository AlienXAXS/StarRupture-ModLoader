#include "pch.h"
#include "actor_begin_play.h"
#include "../../logger.h"
#include "../../scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::ActorBeginPlay
{
	// AActor::BeginPlay(AActor* this)
	typedef void(__fastcall* AActor_BeginPlay_t)(void* thisPtr);

	static Hook g_hook;
	static AActor_BeginPlay_t g_original = nullptr;

	// Callback for plugins to receive actor-begin-play events
	static std::vector<PluginActorBeginPlayCallback> g_pluginCallbacks;

	static void __fastcall Detour(void* thisPtr)
	{
		// Call original first so the actor is fully initialised before we notify
		if (g_original)
		{
			g_original(thisPtr);
		}

		// Notify registered plugins
		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (!g_pluginCallbacks[i])
				continue;

			try
			{
				g_pluginCallbacks[i](thisPtr);
			}
			catch (const std::exception& e)
			{
				ModLoader::LogError(L"[ActorBeginPlay] Exception in callback: %S", e.what());
			}
			catch (...)
			{
				ModLoader::LogError(L"[ActorBeginPlay] Unknown exception in callback");
			}
		}
	}

	bool Install()
	{
		ModLoader::LogInfo(L"[ActorBeginPlay] Installing hook...");

		// The pattern matches unique interior bytes inside AActor::BeginPlay,
		// NOT the function prologue.  We find the interior match first, then
		// walk backwards to locate the actual function entry point.
		uintptr_t interiorAddr = Scanner::FindPatternInMainModule(ScanPatterns::AActor_BeginPlay);

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		if (!interiorAddr)
		{
			ModLoader::LogError(L"[ActorBeginPlay] AActor::BeginPlay interior pattern not found");
			return false;
		}

		ModLoader::LogInfo(L"[ActorBeginPlay] Interior pattern matched at 0x%llX (base+0x%llX) - "
			L"reverse-scanning for function prologue...",
			static_cast<unsigned long long>(interiorAddr),
			static_cast<unsigned long long>(interiorAddr - base));

		// Walk backwards to find the function entry point.
		// x64 functions are aligned and preceded by padding bytes:
		//   CC   = int3  (most common inter-function padding)
		//   90= NOP
		//   66 90    = 2-byte NOP (used for alignment)
		//   C3       = RET from the previous function
		// We scan backwards looking for the transition from padding/ret
		// into real code — that boundary is the function entry point.
		static constexpr size_t maxScanBack = 256;

		uintptr_t addr = 0;
		for (size_t offset = 1; offset <= maxScanBack; ++offset)
		{
			const uint8_t* c = reinterpret_cast<const uint8_t*>(interiorAddr - offset);

			ModLoader::LogTrace(L"[ActorBeginPlay]   -%3zu: %02X %02X %02X %02X",
				offset, c[0], c[1], c[2], c[3]);

			// Current byte is NOT padding, but the byte before it IS padding or RET.
			// That means we've found the first instruction of the function.
			bool currentIsPadding = (c[0] == 0xCC || c[0] == 0x90);

			if (!currentIsPadding && offset > 1)
			{
				uint8_t prevByte = *reinterpret_cast<const uint8_t*>(interiorAddr - offset - 1);
				if (prevByte == 0xCC || prevByte == 0x90 || prevByte == 0xC3)
				{
					addr = interiorAddr - offset;
					ModLoader::LogTrace(L"[ActorBeginPlay]   ^ function entry at offset -%zu "
						L"(prev byte: %02X, first instr byte: %02X)", offset, prevByte, c[0]);
					break;
				}

				// Check for 2-byte NOP (66 90) preceding
				if (offset > 2)
				{
					const uint8_t* prev2 = reinterpret_cast<const uint8_t*>(interiorAddr - offset - 2);
					if (prev2[0] == 0x66 && prev2[1] == 0x90)
					{
						addr = interiorAddr - offset;
						ModLoader::LogTrace(L"[ActorBeginPlay]   ^ function entry at offset -%zu "
							L"(prev bytes: 66 90, first instr byte: %02X)", offset, c[0]);
						break;
					}
				}
			}
		}

		if (!addr)
		{
			ModLoader::LogError(L"[ActorBeginPlay] Failed to find function entry point "
				L"(padding boundary) within %zu bytes before interior match",
				maxScanBack);
			return false;
		}

		ModLoader::LogInfo(L"[ActorBeginPlay] AActor::BeginPlay entry at 0x%llX (base+0x%llX), "
			L"%lld bytes before interior match",
			static_cast<unsigned long long>(addr),
			static_cast<unsigned long long>(addr - base),
			static_cast<long long>(interiorAddr - addr));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoader::LogInfo(L"[ActorBeginPlay] Hook installed successfully");
		else
			ModLoader::LogError(L"[ActorBeginPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoader::LogInfo(L"[ActorBeginPlay] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginActorBeginPlayCallback callback)
	{
		if (!callback)
		{
			ModLoader::LogWarn(L"[ActorBeginPlay] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoader::LogInfo(L"[ActorBeginPlay] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoader::LogError(L"[ActorBeginPlay] Failed to install hook for actor-begin-play callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoader::LogDebug(L"[ActorBeginPlay] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginActorBeginPlayCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoader::LogDebug(L"[ActorBeginPlay] Plugin callback unregistered (%zu remaining)", g_pluginCallbacks.size());
		}
	}
}
