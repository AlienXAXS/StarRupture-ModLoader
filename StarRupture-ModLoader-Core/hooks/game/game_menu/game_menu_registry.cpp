#include "pch.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "game_menu_registry.h"
#include "game_menu.h"
#include "logging/logger.h"

#include <mutex>
#include <string>
#include <cstring>

namespace GameMenu::Registry
{
	namespace
	{
		struct Entry
		{
			bool                   live     = false;
			std::string            owner;   // plugin name; empty means the loader
			std::string            id;
			std::string            label;
			int                    targets  = 0;
			int                    anchor   = PLUGIN_GAME_MENU_ANCHOR_BOTTOM;
			PluginGameMenuCallback onClick  = nullptr;
			void*                  userData = nullptr;
		};

		std::mutex s_mutex;
		Entry      s_entries[kMaxEntries];
		int        s_order[kMaxEntries]{};   // live slots, in registration order
		int        s_orderCount = 0;

		// Handles are slot+1 rather than Entry*, so a handle held past an
		// unload names a dead slot instead of freed memory.
		GameMenuEntryHandle SlotToHandle(int slot)
		{
			return reinterpret_cast<GameMenuEntryHandle>(static_cast<uintptr_t>(slot) + 1);
		}

		int HandleToSlot(GameMenuEntryHandle handle)
		{
			const uintptr_t raw = reinterpret_cast<uintptr_t>(handle);
			if (raw == 0 || raw > static_cast<uintptr_t>(kMaxEntries))
				return -1;
			return static_cast<int>(raw) - 1;
		}

		void RemoveFromOrderLocked(int slot)
		{
			int write = 0;
			for (int read = 0; read < s_orderCount; ++read)
			{
				if (s_order[read] != slot)
					s_order[write++] = s_order[read];
			}
			s_orderCount = write;
		}

		void ClearSlotLocked(int slot)
		{
			s_entries[slot] = Entry{};
			RemoveFromOrderLocked(slot);
		}

		// -------------------------------------------------------------------
		// A callback address that no longer belongs to a loaded module cannot
		// be valid, whatever the plugin did or failed to do, so refuse it here
		// rather than trusting ForgetPlugin to have run.  The try/catch at the
		// call site does not help: under /EHsc an access violation is not a
		// C++ exception.
		//
		// Checked only for the entry actually being clicked -- one per click,
		// not a sweep -- because GetModuleHandleEx touches the loader lock.
		// -------------------------------------------------------------------
		bool CallbackStillMapped(const void* callback)
		{
			if (!callback)
				return false;

			HMODULE owner = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                        static_cast<LPCSTR>(callback), &owner))
				return false;

			return owner != nullptr;
		}

		const wchar_t* TargetsText(int targets)
		{
			const bool main  = (targets & PLUGIN_GAME_MENU_MAIN)  != 0;
			const bool pause = (targets & PLUGIN_GAME_MENU_PAUSE) != 0;
			if (main && pause) return L"main+pause";
			if (main)          return L"main";
			if (pause)         return L"pause";
			return L"none";
		}

		bool ValidDesc(const PluginGameMenuEntryDesc* desc)
		{
			if (!desc)                                  return false;
			if (!desc->id      || !desc->id[0])         return false;
			if (!desc->label   || !desc->label[0])      return false;
			if (!desc->onClick)                         return false;
			if ((desc->targets & (PLUGIN_GAME_MENU_MAIN | PLUGIN_GAME_MENU_PAUSE)) == 0)
				return false;
			if (desc->anchor < PLUGIN_GAME_MENU_ANCHOR_TOP ||
			    desc->anchor > PLUGIN_GAME_MENU_ANCHOR_BOTTOM)
				return false;
			return true;
		}

		// Interface strings are copied for the same reason a ConfigSchema had
		// to be forgotten: the literals live in the plugin's module.
		GameMenuEntryHandle AddLocked(const char* owner, const PluginGameMenuEntryDesc* desc)
		{
			for (int i = 0; i < s_orderCount; ++i)
			{
				const Entry& e = s_entries[s_order[i]];
				if (e.owner == (owner ? owner : "") && e.id == desc->id)
				{
					ModLoaderLogger::LogWarn(
						L"[GameMenu] '%S' already registered a menu entry with id '%S' -- ignoring the second one",
						owner ? owner : "ModLoader", desc->id);
					return nullptr;
				}
			}

			int slot = -1;
			for (int i = 0; i < kMaxEntries; ++i)
			{
				if (!s_entries[i].live) { slot = i; break; }
			}
			if (slot < 0)
			{
				ModLoaderLogger::LogError(
					L"[GameMenu] All %d menu entry slots are in use -- rejecting '%S'",
					kMaxEntries, desc->id);
				return nullptr;
			}

			Entry& e   = s_entries[slot];
			e.live     = true;
			e.owner    = owner ? owner : "";
			e.id       = desc->id;
			e.label    = desc->label;
			e.targets  = desc->targets;
			e.anchor   = desc->anchor;
			e.onClick  = desc->onClick;
			e.userData = desc->userData;

			s_order[s_orderCount++] = slot;
			return SlotToHandle(slot);
		}

		// -------------------------------------------------------------------
		// Plugin-facing entry points
		// -------------------------------------------------------------------
		GameMenuEntryHandle IfaceAddEntry(const IPluginSelf* self, const PluginGameMenuEntryDesc* desc)
		{
			if (!self || !self->name)
			{
				ModLoaderLogger::LogWarn(L"[GameMenu] AddEntry called without a valid IPluginSelf");
				return nullptr;
			}
			if (!ValidDesc(desc))
			{
				ModLoaderLogger::LogWarn(L"[GameMenu] '%S' passed an incomplete menu entry -- ignored",
				                         self->name);
				return nullptr;
			}
			if (!::Hooks::GameMenu::IsInstalled())
			{
				ModLoaderLogger::LogWarn(
					L"[GameMenu] '%S' registered '%S' but the menu hooks are not installed -- "
					L"the entry will not appear. Check IsAvailable() first.",
					self->name, desc->id);
			}

			std::lock_guard<std::mutex> lock(s_mutex);
			GameMenuEntryHandle handle = AddLocked(self->name, desc);
			if (handle)
			{
				ModLoaderLogger::LogDebug(
					L"[GameMenu] '%S' added menu entry '%S' (\"%S\") targets=%s anchor=%d",
					self->name, desc->id, desc->label, TargetsText(desc->targets), desc->anchor);
			}
			return handle;
		}

		bool IfaceRemoveEntry(const IPluginSelf* self, GameMenuEntryHandle handle)
		{
			if (!self || !self->name)
				return false;

			const int slot = HandleToSlot(handle);
			if (slot < 0)
				return false;

			std::lock_guard<std::mutex> lock(s_mutex);
			Entry& e = s_entries[slot];
			if (!e.live || e.owner != self->name)
				return false;

			ModLoaderLogger::LogDebug(L"[GameMenu] '%S' removed menu entry '%S'",
			                          self->name, e.id.c_str());
			ClearSlotLocked(slot);
			return true;
		}

		int IfaceRemoveAllEntries(const IPluginSelf* self)
		{
			if (!self || !self->name)
				return 0;

			std::lock_guard<std::mutex> lock(s_mutex);
			int removed = 0;
			for (int i = 0; i < kMaxEntries; ++i)
			{
				if (s_entries[i].live && s_entries[i].owner == self->name)
				{
					ClearSlotLocked(i);
					++removed;
				}
			}
			return removed;
		}

		bool IfaceIsAvailable()
		{
			return ::Hooks::GameMenu::IsInstalled();
		}

		IPluginGameMenu g_interface = {
			IfaceAddEntry,
			IfaceRemoveEntry,
			IfaceRemoveAllEntries,
			IfaceIsAvailable
		};
	}

	IPluginGameMenu* GetInterface()
	{
		return &g_interface;
	}

	GameMenuEntryHandle AddLoaderEntry(const PluginGameMenuEntryDesc* desc)
	{
		if (!ValidDesc(desc))
		{
			ModLoaderLogger::LogWarn(L"[GameMenu] AddLoaderEntry called with an incomplete descriptor");
			return nullptr;
		}

		std::lock_guard<std::mutex> lock(s_mutex);
		GameMenuEntryHandle handle = AddLocked(nullptr, desc);
		if (handle)
		{
			ModLoaderLogger::LogDebug(
				L"[GameMenu] Registered loader menu entry '%S' (\"%S\") targets=%s anchor=%d",
				desc->id, desc->label, TargetsText(desc->targets), desc->anchor);
		}
		return handle;
	}

	bool RemoveLoaderEntry(GameMenuEntryHandle handle)
	{
		const int slot = HandleToSlot(handle);
		if (slot < 0)
			return false;

		std::lock_guard<std::mutex> lock(s_mutex);
		if (!s_entries[slot].live || !s_entries[slot].owner.empty())
			return false;

		ClearSlotLocked(slot);
		return true;
	}

	int Snapshot(int target, EntryView* out, int maxOut)
	{
		if (!out || maxOut <= 0)
			return 0;

		std::lock_guard<std::mutex> lock(s_mutex);

		int written = 0;
		for (int i = 0; i < s_orderCount && written < maxOut; ++i)
		{
			const Entry& e = s_entries[s_order[i]];
			if (!e.live || (e.targets & target) == 0)
				continue;

			EntryView& v = out[written++];
			v.slot   = s_order[i];
			v.anchor = e.anchor;
			strncpy_s(v.label, sizeof(v.label), e.label.c_str(), _TRUNCATE);
		}
		return written;
	}

	bool HasAnyFor(int target)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		for (int i = 0; i < s_orderCount; ++i)
		{
			const Entry& e = s_entries[s_order[i]];
			if (e.live && (e.targets & target) != 0)
				return true;
		}
		return false;
	}

	void Invoke(int slot)
	{
		if (slot < 0 || slot >= kMaxEntries)
			return;

		PluginGameMenuCallback callback = nullptr;
		void*                  userData = nullptr;
		std::string            owner;
		std::string            id;

		{
			std::lock_guard<std::mutex> lock(s_mutex);
			const Entry& e = s_entries[slot];
			if (!e.live)
			{
				// The entry was removed between the menu being built and the
				// click -- a plugin unloaded while its menu was on screen.
				ModLoaderLogger::LogDebug(
					L"[GameMenu] Menu entry slot %d is no longer registered -- ignoring the click", slot);
				return;
			}
			callback = e.onClick;
			userData = e.userData;
			owner    = e.owner;
			id       = e.id;
		}

		// Loader-owned callbacks live in this DLL, which is still mapped by
		// definition -- we are executing in it.  A plugin's is worth checking.
		if (!owner.empty() && !CallbackStillMapped(reinterpret_cast<const void*>(callback)))
		{
			ModLoaderLogger::LogError(
				L"[GameMenu] Dropping menu entry '%S' from '%S': its callback at %p is no longer in a "
				L"loaded module. The plugin was unloaded without removing it; calling it would have "
				L"crashed the game.",
				id.c_str(), owner.c_str(), reinterpret_cast<const void*>(callback));

			std::lock_guard<std::mutex> lock(s_mutex);
			ClearSlotLocked(slot);
			return;
		}

		ModLoaderLogger::LogInfo(L"[GameMenu] Menu entry '%S' clicked (owner: %S)",
		                         id.c_str(), owner.empty() ? "ModLoader" : owner.c_str());

		try
		{
			callback(userData);
		}
		catch (const std::exception& e)
		{
			ModLoaderLogger::LogError(L"[GameMenu] Exception in menu entry '%S': %S", id.c_str(), e.what());
		}
		catch (...)
		{
			ModLoaderLogger::LogError(L"[GameMenu] Unknown exception in menu entry '%S'", id.c_str());
		}
	}

	void ForgetPlugin(const char* pluginName)
	{
		if (!pluginName || !pluginName[0])
			return;

		std::lock_guard<std::mutex> lock(s_mutex);
		int removed = 0;
		for (int i = 0; i < kMaxEntries; ++i)
		{
			if (s_entries[i].live && s_entries[i].owner == pluginName)
			{
				ClearSlotLocked(i);
				++removed;
			}
		}

		if (removed > 0)
		{
			ModLoaderLogger::LogDebug(L"[GameMenu] Dropped %d menu entr%s belonging to '%S'",
			                          removed, removed == 1 ? L"y" : L"ies", pluginName);
		}
	}
}

#endif // MODLOADER_CLIENT_BUILD
