#include "pch.h"
#include "plugins/plugin_call_gate.h"

#include "logging/logger.h"
#include "plugins/plugin_guard.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace PluginCallGate
{
	namespace
	{
		std::mutex              s_mutex;
		std::condition_variable s_changed;
		int                     s_active = 0;      // threads holding a Scope
		DWORD                   s_closer = 0;      // thread freeing a module, 0 while open
		thread_local int        t_depth = 0;

		void UntrackAndFree(HMODULE module)
		{
			PluginGuard::Untrack(module);
			FreeLibrary(module);
		}

		std::atomic<FreeStep> s_freeStep{ &UntrackAndFree };

		void Open()
		{
			s_closer = 0;
			s_changed.notify_all();
		}
	}

	Scope::Scope()
	{
		if (t_depth == 0)
		{
			std::unique_lock<std::mutex> lock(s_mutex);
			// The closer passes: freeing runs the module's DllMain on its thread.
			s_changed.wait(lock, [] { return !s_closer || s_closer == GetCurrentThreadId(); });
			++s_active;
		}
		++t_depth;
	}

	Scope::~Scope()
	{
		if (--t_depth != 0) return;
		std::lock_guard<std::mutex> lock(s_mutex);
		--s_active;
		s_changed.notify_all();
	}

	bool InsideCall() { return t_depth > 0; }

	void SetFreeStep(FreeStep step) { s_freeStep.store(step ? step : &UntrackAndFree); }

	bool FreeWhenIdle(HMODULE module, const char* name, DWORD timeoutMs)
	{
		if (!module) return true;
		if (!name) name = "?";
		if (InsideCall())
		{
			ModLoaderLogger::LogError(L"[PluginCallGate] '%S' stays loaded: it cannot be freed from inside a plugin call",
			                          name);
			return false;
		}

		bool idle = false;
		{
			std::unique_lock<std::mutex> lock(s_mutex);
			if (!s_closer)
			{
				s_closer = GetCurrentThreadId();
				idle = s_changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return s_active == 0; });
				if (!idle)
					Open();
			}
		}
		// Logged with the gate open: a log sink may itself be waiting at it.
		if (!idle)
		{
			ModLoaderLogger::LogError(L"[PluginCallGate] '%S' stays loaded: plugin calls were still running after "
			                          L"%lu ms (or another unload was freeing a module). Try again.",
			                          name, static_cast<unsigned long>(timeoutMs));
			return false;
		}

		s_freeStep.load()(module);

		std::lock_guard<std::mutex> lock(s_mutex);
		Open();
		return true;
	}
}
