#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "plugins/reload_policy.h"

// ---------------------------------------------------------------------------
// CrashRestart
//
// What happens to a plugin after the crash guard (plugins/plugin_guard.h)
// switched it off: a short wait, then the same reload the Plugins tab's RELOAD
// button runs, on the game thread, if the plugin's load and unload settings
// allow one now. At most kMaxRestarts per plugin per session; after that it
// stays off until the game restarts.
//
// The guard's reporter calls NoteCrash on whichever thread faulted, so that is
// all it does: no plugin lock, no settings, no UI. The plugin manager's game
// thread pump (PluginManager::PumpCrashRestarts, after GameThreadDispatch in
// the engine tick) takes the entries that are ready and acts on them.
//
// Header only and free of engine and game state, so the tests build it as is.
// ---------------------------------------------------------------------------
namespace CrashRestart
{
	inline constexpr int      kMaxRestarts = 3;      // per plugin, per session
	inline constexpr uint64_t kDelayMs     = 3000;   // from the crash (or the menu) to the reload

	// What to do with a crashed plugin, given its settings and the session.
	enum class Plan
	{
		RestartNow,        // reload it (after the delay)
		WaitForMenu,       // a MENU setting and a game world is running: reload at the main menu
		NeedsGameRestart,  // a RESTART setting: no reload while the game runs
		GaveUp,            // restarted kMaxRestarts times already this session
		Disabled,          // the "Restart crashed plugins automatically" setting is off
	};

	// The same rules as ReloadPolicy::ReloadRefusal for a loaded plugin, with no
	// busy reason (the plugin manager ignores it once a plugin has crashed).
	// RESTART comes first: no setting and no count can make RELOAD possible.
	inline Plan Decide(const ReloadPolicy::Settings& s, bool worldRunning, int restartsSoFar, bool enabled)
	{
		if (s.unload == PluginReload::RESTART || s.load == PluginReload::RESTART)
			return Plan::NeedsGameRestart;
		if (!enabled)
			return Plan::Disabled;
		if (restartsSoFar >= kMaxRestarts)
			return Plan::GaveUp;
		if (worldRunning && (s.unload == PluginReload::MENU || s.load == PluginReload::MENU))
			return Plan::WaitForMenu;
		return Plan::RestartNow;
	}

	// The toast and log text for a plan, when the crash is announced.
	inline std::string Announcement(Plan plan, const std::string& name, const std::string& callback)
	{
		const std::string head = name + " crashed (" + callback + ")";
		switch (plan)
		{
		case Plan::RestartNow:       return head + " and was switched off. Restarting it in a moment.";
		case Plan::WaitForMenu:      return head + " and was switched off. It restarts when you return to the main menu.";
		case Plan::NeedsGameRestart: return head + " and was switched off. Restart the game to turn it back on.";
		case Plan::GaveUp:           return head + " again. It stays off until the game restarts.";
		case Plan::Disabled:         return head + " and was switched off. RELOAD it on the Plugins tab to turn it back on.";
		}
		return head;
	}

	inline std::string Restarted(const std::string& name, const std::string& callback)
	{
		return name + " crashed (" + callback + ") and was restarted";
	}

	inline std::string RestartFailed(const std::string& name, const std::string& callback, const std::string& why)
	{
		return name + " crashed (" + callback + ") and could not be restarted: " +
		       (why.empty() ? std::string("see ModLoader.log") : why);
	}

	enum class Stage
	{
		New,             // just crashed: decide and announce
		Scheduled,       // reload once dueMs has passed
		WaitingForMenu,  // reload once no game world is running
	};

	struct Entry
	{
		uintptr_t   module = 0;   // the crashed instance's HMODULE: a reload or unload since makes it stale
		std::string name;
		std::string callback;
		Stage       stage = Stage::New;
		uint64_t    dueMs = 0;
	};

	class Scheduler
	{
	public:
		// Any thread. The guard reports a plugin once per load, so one entry per
		// module; a second report for the same module is dropped.
		void NoteCrash(uintptr_t module, const char* name, const char* callback, uint64_t nowMs)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stopped)
				return;
			for (const Entry& e : m_entries)
				if (e.module == module)
					return;
			Entry e;
			e.module   = module;
			e.name     = name ? name : "(unknown)";
			e.callback = callback ? callback : "a callback";
			e.dueMs    = nowMs;
			m_entries.push_back(std::move(e));
			m_pending.store(true, std::memory_order_release);
		}

		// One atomic load: the pump's cost on a tick with nothing to do.
		bool HasWork() const { return m_pending.load(std::memory_order_acquire); }

		// Game thread. Removes and returns the entries to act on now: every New
		// one, a Scheduled one whose time has come, a WaitingForMenu one once no
		// world is running. A crash noted while the caller works through them is
		// not among them, so nothing runs twice in one pass.
		std::vector<Entry> TakeReady(uint64_t nowMs, bool worldRunning)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			std::vector<Entry> ready;
			if (m_stopped)
				return ready;
			for (auto it = m_entries.begin(); it != m_entries.end(); )
			{
				const bool take = it->stage == Stage::New
				               || (it->stage == Stage::Scheduled && nowMs >= it->dueMs)
				               || (it->stage == Stage::WaitingForMenu && !worldRunning);
				if (take) { ready.push_back(std::move(*it)); it = m_entries.erase(it); }
				else ++it;
			}
			m_pending.store(!m_entries.empty(), std::memory_order_release);
			return ready;
		}

		// Game thread. Puts an entry back for a later pass.
		void Requeue(Entry e, Stage stage, uint64_t dueMs)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_stopped)
				return;
			e.stage = stage;
			e.dueMs = dueMs;
			m_entries.push_back(std::move(e));
			m_pending.store(true, std::memory_order_release);
		}

		// How many times this plugin was restarted this session (by name: the
		// module changes with every reload).
		int Restarts(const std::string& name) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_restarts.find(name);
			return it == m_restarts.end() ? 0 : it->second;
		}

		void CountRestart(const std::string& name)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_restarts[name];
		}

		// The game is exiting: drop everything and accept nothing more, so no
		// reload ever runs during or after the shutdown.
		void Stop()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stopped = true;
			m_entries.clear();
			m_pending.store(false, std::memory_order_release);
		}

		bool Stopped() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_stopped;
		}

	private:
		mutable std::mutex                   m_mutex;
		std::vector<Entry>                   m_entries;
		std::unordered_map<std::string, int> m_restarts;
		std::atomic<bool>                    m_pending{ false };
		bool                                 m_stopped = false;
	};

	// The loader's one scheduler.
	inline Scheduler& Global()
	{
		static Scheduler s;
		return s;
	}
}
