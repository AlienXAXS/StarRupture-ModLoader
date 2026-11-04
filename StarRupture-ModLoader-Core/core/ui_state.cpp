#include "pch.h"
#include "ui_state.h"
#include "logging/logger.h"
#include "plugins/plugin_guard.h"

#include <atomic>
#include <mutex>
#include <vector>

// Client-only feature -- entire translation unit is a no-op on server/generic builds.
#ifdef MODLOADER_CLIENT_BUILD

namespace UIState
{
	namespace
	{
		struct Entry
		{
			uint64_t              id;        // to find it again after the lock is dropped
			uint64_t              mask;
			PluginUIStateCallback callback;
			void*                 userData;
			uint64_t              seen;      // the state this entry was last compared with
		};

		struct Call
		{
			uint64_t              id;
			PluginUIStateCallback callback;
			void*                 userData;
			uint64_t              oldState;
		};

		std::mutex         s_mutex;
		std::vector<Entry> s_entries;
		uint64_t           s_nextId = 1;

		// Read without the lock on every tick and every query.
		std::atomic<uint64_t> s_state{ 0 };
		std::atomic<size_t>   s_count{ 0 };
		std::atomic<bool>     s_menuReadDisabled{ false };  // DisableMenuRead
		std::atomic<uint64_t> s_wantedMenuBits{ 0 };  // menu bits any registration asks for

		// After any change to s_entries, under s_mutex.
		void Recount()
		{
			uint64_t wanted = 0;
			for (const Entry& e : s_entries)
				wanted |= e.mask;
			s_count.store(s_entries.size(), std::memory_order_relaxed);
			s_wantedMenuBits.store(wanted & kMenuBits, std::memory_order_relaxed);
		}

		bool StillRegistered(uint64_t id)
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			for (const Entry& e : s_entries)
				if (e.id == id)
					return true;
			return false;
		}
	}

	uint64_t MenuBits(size_t widgetCount, const MenuKind* kinds, size_t kindCount)
	{
		if (widgetCount == 0)
			return 0;

		uint64_t bits = PLUGIN_UI_ANY_MENU;
		for (size_t i = 0; i < kindCount; ++i)
		{
			switch (kinds[i])
			{
			case MenuKind::Map:         bits |= PLUGIN_UI_MAP;          break;
			case MenuKind::Inventory:   bits |= PLUGIN_UI_INVENTORY;    break;
			case MenuKind::BuildMenu:   bits |= PLUGIN_UI_BUILD_MENU;   break;
			case MenuKind::StationMenu: bits |= PLUGIN_UI_STATION_MENU; break;
			case MenuKind::Other:       break;
			}
		}
		return bits;
	}

	uint64_t Get()
	{
		// The menu bits are read only while a registration wants them, so
		// here they could be stale or not read yet: a query never sees them.
		return s_state.load(std::memory_order_acquire) & ~kMenuBits;
	}

	bool MenuReadWanted()
	{
		return !s_menuReadDisabled.load(std::memory_order_acquire)
			&& s_wantedMenuBits.load(std::memory_order_relaxed) != 0;
	}

	void DisableMenuRead()
	{
		s_menuReadDisabled.store(true, std::memory_order_release);
	}

	void Register(uint64_t mask, PluginUIStateCallback callback, void* userData)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[UIState] RegisterOnUIStateChanged: null callback ignored");
			return;
		}

		std::lock_guard<std::mutex> lock(s_mutex);
		for (const Entry& e : s_entries)
		{
			if (e.callback == callback && e.userData == userData)
			{
				ModLoaderLogger::LogWarn(L"[UIState] RegisterOnUIStateChanged: callback %p / userData %p "
				                         L"is already registered -- ignored",
				                         reinterpret_cast<const void*>(callback), userData);
				return;
			}
		}
		s_entries.push_back({ s_nextId++, mask, callback, userData, 0 });
		Recount();
		ModLoaderLogger::LogDebug(L"[UIState] Registered callback %p for 0x%llX (%zu total)",
		                          reinterpret_cast<const void*>(callback),
		                          static_cast<unsigned long long>(mask), s_entries.size());
	}

	void Unregister(PluginUIStateCallback callback, void* userData)
	{
		if (!callback) return;

		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto it = s_entries.begin(); it != s_entries.end(); ++it)
		{
			if (it->callback == callback && it->userData == userData)
			{
				s_entries.erase(it);
				Recount();
				ModLoaderLogger::LogDebug(L"[UIState] Unregistered callback %p (%zu left)",
				                          reinterpret_cast<const void*>(callback), s_entries.size());
				return;
			}
		}
		ModLoaderLogger::LogWarn(L"[UIState] UnregisterOnUIStateChanged: callback %p / userData %p "
		                         L"was not registered",
		                         reinterpret_cast<const void*>(callback), userData);
	}

	void Publish(uint64_t state)
	{
		s_state.store(state, std::memory_order_release);
		if (s_count.load(std::memory_order_relaxed) == 0)
			return;

		// Decide under the lock, call without it: a callback may register or
		// unregister (itself included).
		std::vector<Call> calls;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			for (Entry& e : s_entries)
			{
				const uint64_t old = e.seen;
				e.seen = state;
				if ((old ^ state) & e.mask)
					calls.push_back({ e.id, e.callback, e.userData, old });
			}
		}

		for (const Call& c : calls)
		{
			// One removed by an earlier callback this tick is not called.
			if (calls.size() > 1 && !StillRegistered(c.id))
				continue;
			try { PluginGuard::Invoke(c.callback, "UI state callback", c.oldState, state, c.userData); }
			catch (...)
			{
				ModLoaderLogger::LogError(L"[UIState] UI state callback %p threw",
				                          reinterpret_cast<const void*>(c.callback));
			}
		}
	}

	void ForgetModule(HMODULE module)
	{
		if (!module) return;

		std::lock_guard<std::mutex> lock(s_mutex);
		const size_t dropped = PluginGuard::ForgetModule(s_entries, module,
			[](const Entry& e) { return e.callback; });
		Recount();
		if (dropped)
			ModLoaderLogger::LogDebug(L"[UIState] Dropped %zu UI state callback(s) of an unloading plugin", dropped);
	}
}

#endif // MODLOADER_CLIENT_BUILD
