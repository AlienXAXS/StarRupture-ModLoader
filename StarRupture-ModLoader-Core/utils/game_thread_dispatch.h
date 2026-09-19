#pragma once

#include <functional>
#include <future>
#include <string>

// ---------------------------------------------------------------------------
// GameThreadDispatch -- lightweight game-thread task queue (modloader core)
//
// Any code running on a background thread (RCON handlers, network callbacks,
// etc.) that needs to touch engine state must execute on the game thread.
// This utility provides a thread-safe queue drained every frame by the
// engine-tick hook (engine_tick.cpp).
//
// Plugin authors access this via hooks->Engine->PostToGameThread(fn, ctx).
//
// Two internal overloads are provided:
//   PostVoid(fn)   -- fire-and-forget void callable (used by plugin API)
//   PostString(fn) -- returns std::future<std::string> (used internally by RCON)
// ---------------------------------------------------------------------------
namespace GameThreadDispatch
{
    // Post a void callable.  Returns immediately.  The callable is invoked on
    // the game thread during the next engine tick.
    // Thread-safe; may be called from any thread.
    void PostVoid(std::function<void()> fn);

    // Post a callable that returns std::string.  Returns a future the calling
    // thread can block on.  Thread-safe; may be called from any thread.
    std::future<std::string> PostString(std::function<std::string()> fn);

    // Execute all queued tasks on the calling thread.
    // MUST be called from the game thread only.
    // Called automatically by engine_tick.cpp every frame.
    void Drain();

    // True when the caller is already on the game thread, so engine state can
    // be touched directly instead of being posted.
    //
    // The game thread is identified by whichever thread called Drain() last,
    // which is the engine-tick hook. That means this returns false for every
    // thread -- including the real game thread -- until the first tick has run.
    // Treat a false result as "post it", never as "this is definitely a worker
    // thread": posting is always safe, and one deferred frame during startup is
    // not worth a special case.
    bool IsGameThread();

    // True once Drain() has run at least once, i.e. once the game thread has
    // an identity and IsGameThread() means something. Before that point the
    // engine has not ticked: the main thread is still inside startup (parked
    // or held by the loader's init hooks) and nothing else is touching engine
    // state, so a caller that must run "on the game thread" can run inline
    // rather than queue behind a tick that has not started.
    bool HasTicked();
}
