#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// UGameEngine::Tick Hook
//
// Purpose: Fires every frame on the game thread.  Plugins can register
//     callbacks here to perform periodic work that must run on the
//      game thread (e.g. draining task queues, polling state, etc.)
//
// Hook point:
//   UGameEngine::Tick(UGameEngine* this, float DeltaSeconds, bool bIdleMode)
//
// Performance note: callbacks are invoked every frame � keep them fast.
// ---------------------------------------------------------------------------

namespace Hooks::EngineTick
{
	// Callback signature for plugins
	using PluginEngineTickCallback = void(*)(float deltaSeconds);

	// Install the hook
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Register a plugin callback to be notified every game-thread tick
	void RegisterPluginCallback(PluginEngineTickCallback callback);

	// Unregister a plugin callback
	void UnregisterPluginCallback(PluginEngineTickCallback callback);

	// Enable/disable the tick-stutter warning log at runtime (default: disabled).
	// Has no effect on the tick itself -- only on whether outliers get logged.
	void SetStutterLoggingEnabled(bool enabled);
	bool IsStutterLoggingEnabled();

	// Enable/disable stack sampling of the engine tick call (default: disabled).
	// Heavier than plain stutter logging -- suspends/resumes the game thread
	// at ~1kHz for the duration of every profiled tick's engine call so that
	// if the tick turns out to be a stutter, tick_profiler.h's StutterEvent
	// carries a ranked Module+Offset hotspot list (see stack_sampler.h). Has
	// no effect unless stutter logging is also enabled.
	void SetStackSamplingEnabled(bool enabled);
	bool IsStackSamplingEnabled();

	// Returns the trampoline address of the original UGameEngine::Tick, or 0 if not yet installed.
	// Cast to: void(__fastcall*)(void* thisPtr, float deltaSeconds, bool bIdleMode)
	uintptr_t GetOriginalPtr();
}
