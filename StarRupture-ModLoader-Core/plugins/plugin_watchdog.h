#pragma once

#include <windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// PluginWatchdog
//
// Names the plugin when the game hangs inside one of its callbacks.
//
// A plugin that blocks in its tick or render callback freezes the game with
// nothing in the log: the thread that would write the next line is the one
// that is stuck, and a hang produces no crash dialog or dump. So every call
// into plugin code from the engine tick and the panel/widget registries is
// marked with a Scope, and a threadpool timer checks the marks once a second.
// A call still running after kStuckAfterMs gets one warning naming the DLL and
// the callback; if it does return, one more line says how long it took.
//
// Cheap enough for every frame: a Scope only writes a few atomics into a slot
// owned by the calling thread. Nothing is logged unless a call is stuck.
// ---------------------------------------------------------------------------

namespace PluginWatchdog
{
    // Starts the periodic check. Call once, during startup.
    void Start();

    // Stops the check and waits for one in progress. Call once no plugin code
    // can be called any more.
    void Stop();

    // A call into plugin code, as recorded in the calling thread's slot.
    struct Call
    {
        uint64_t    id      = 0;       // 0 while no call is running
        const void* code    = nullptr;
        const char* what    = nullptr;
        ULONGLONG   startMs = 0;
    };

    struct Slot;

    // One call into plugin code on the current thread, for the Scope's
    // lifetime. `code` is the plugin's callback (any address inside its DLL
    // will do); `what` names the kind of call and must be a string literal,
    // since the timer reads it from another thread at any time.
    class Scope
    {
    public:
        Scope(const void* code, const char* what);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Slot*        m_slot;
        Call         m_call;
        Call         m_outer;  // the call this one interrupted, restored on exit
    };
}
