#pragma once

#include <functional>
#include <future>
#include <string>

// ---------------------------------------------------------------------------
// GameThreadDispatch — lightweight game-thread task queue
//
// RCON command handlers run on network threads.  Any command that needs to
// touch engine state (UObject lookups, save subsystem, etc.) must execute on
// the game thread to be thread-safe.
//
// Usage (from a command handler):
//
//   auto fut = GameThreadDispatch::Post([]() -> std::string {
//       // engine-safe work here
//       return "done\n";
//   });
//   using namespace std::chrono_literals;
//   if (fut.wait_for(10s) == std::future_status::ready)
//       return fut.get();
//   return "Queued (game thread has not drained yet).\n";
//
// The queue is drained from Rcon::OnAnyWorldBeginPlay and
// Rcon::OnExperienceLoadComplete, which both run on the game thread.
// ---------------------------------------------------------------------------
namespace GameThreadDispatch
{
    // Post a callable that returns std::string.  Returns a future the calling
    // thread can block on.  Thread-safe; may be called from any thread.
    std::future<std::string> Post(std::function<std::string()> fn);

    // Execute all queued tasks on the calling thread.
    // MUST be called from the game thread only.
    void Drain();
}
