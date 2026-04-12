#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Engine Shutdown Hooks
//
// Purpose: Engine shutdown signal - fires before the UObject system tears down.
//          Plugins should use this to restore any patches or free any allocations
//          that reference engine-owned memory (UStruct chains, UObject pointers, etc.)
//
// Hook points (tried in order, only one notification fires):
//   1. FEngineLoop::Exit  (primary)
//   2. UEngine::PreExit   (fallback)
// ---------------------------------------------------------------------------

namespace Hooks::EngineShutdown
{
	// Callback signature for plugins
	using PluginEngineShutdownCallback = void(*)();

	// Install the hook
	// Returns true if the hook was successfully installed
	bool Install();

	// Remove the hook
	void Remove();

	// Register a plugin callback to be notified when engine shuts down
	void RegisterPluginCallback(PluginEngineShutdownCallback callback);

	// Unregister a plugin callback
	void UnregisterPluginCallback(PluginEngineShutdownCallback callback);

	// Returns the trampoline address of the original FEngineLoop::Exit, or 0 if not installed.
	// Cast to: void(__fastcall*)(void* thisPtr)
	uintptr_t GetOriginalPtrEngineLoopExit();

	// Returns the trampoline address of the original UEngine::PreExit, or 0 if not installed.
	// Cast to: void(__fastcall*)(void* thisPtr)
	uintptr_t GetOriginalPtrEnginePreExit();
}
