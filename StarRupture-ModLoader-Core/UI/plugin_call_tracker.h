#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// PluginCallTracker
//
// The panel and widget registries call into plugin code (a renderFn, a
// panel-closed callback) without holding their mutex, so that the plugin can
// call back into the registry from inside it. That leaves a window: the render
// thread is still running in a plugin's code when the game thread, which runs
// every unload, erases the entry and FreeLibrary()s the module.
//
// So each such call is recorded here, under the registry's mutex, for as long
// as it runs, and ForgetModule / Unregister* wait for the matching calls on
// other threads to finish before they return.
//
// The wait is bounded. The unloader holds the plugin manager's lock while it
// waits, and a plugin's render that is itself blocked on that lock (or on the
// game thread) would otherwise hang the game for good. A call on the waiting
// thread itself is never waited for: a renderFn may unregister its own panel.
// ---------------------------------------------------------------------------

namespace UI
{
    class PluginCallTracker
    {
    public:
        // Far longer than any render callback that is merely slow. A call still
        // running after this is stuck, and waiting longer would only hang the game.
        static constexpr DWORD kWaitTimeoutMs = 1000;

        // One call into plugin code. Construct it with the registry's mutex
        // held through `lock`: it records the call and releases the lock for
        // the call's duration, then retakes the lock and removes the record on
        // destruction, so the lock is held again after the scope ends.
        class Scope
        {
        public:
            Scope(PluginCallTracker& tracker, std::unique_lock<std::mutex>& lock,
                  const void* key, HMODULE owner)
                : m_tracker(tracker), m_lock(lock), m_id(++tracker.m_lastId)
            {
                m_tracker.m_calls.push_back({ m_id, key, owner, GetCurrentThreadId() });
                m_lock.unlock();
            }

            ~Scope()
            {
                m_lock.lock();
                for (auto it = m_tracker.m_calls.begin(); it != m_tracker.m_calls.end(); ++it)
                {
                    if (it->id == m_id)
                    {
                        m_tracker.m_calls.erase(it);
                        break;
                    }
                }
                m_tracker.m_done.notify_all();
            }

            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            PluginCallTracker&            m_tracker;
            std::unique_lock<std::mutex>& m_lock;
            const unsigned                m_id;
        };

        // Waits until no other thread is inside a call that match(key, owner)
        // accepts. `lock` holds the registry's mutex on entry and on return.
        // Returns false if it gave up after kWaitTimeoutMs.
        template <typename Match>
        bool WaitForOtherThreads(std::unique_lock<std::mutex>& lock, Match match)
        {
            const DWORD self = GetCurrentThreadId();
            return m_done.wait_for(lock, std::chrono::milliseconds(kWaitTimeoutMs), [&]
            {
                for (const Call& call : m_calls)
                    if (call.thread != self && match(call.key, call.owner))
                        return false;
                return true;
            });
        }

    private:
        struct Call
        {
            unsigned    id;
            const void* key;      // the registry entry or callback being called
            HMODULE     owner;    // module the called code lives in
            DWORD       thread;
        };

        std::vector<Call>       m_calls;
        unsigned                m_lastId = 0;
        std::condition_variable m_done;
    };
}

#endif // MODLOADER_CLIENT_BUILD
