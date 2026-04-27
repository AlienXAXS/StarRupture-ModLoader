#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Engine Initialization Hooks
// 
// Purpose: Engine ready signal - fires when UE5 engine finishes initialization
// Uses multiple hook points as fallbacks for maximum compatibility:
//   1. FEngineLoop::Init (primary)
//   2. UGameEngine::Init (fallback)
// ---------------------------------------------------------------------------

namespace Hooks::EngineInit
{
	// Original function signatures
	using FEngineLoop_Init_t = int32_t(__fastcall *)(void* thisPtr);
	using UGameEngine_Init_t = bool(__fastcall *)(void* thisPtr, void* InEngineLoop);

	// Callback signature for plugins
	using PluginEngineInitCallback = void(*)();

	// Provide the synchronisation events created in DllMain so the detour can
	// signal engine-ready and wait for all plugins to finish loading before
	// letting the original Init proceed.
	void SetSyncEvents(HANDLE engineReadyEvent, HANDLE pluginsLoadedEvent);

	// Provide the event that the detour should signal once it has fully
	// unwound (after NotifyEngineReady returns).  The UE4SS loader thread
	// waits on this instead of a fixed sleep so it never loads while the
	// hook call-stack or GPU driver initialisation is still active.
	void SetUE4SSReadyEvent(HANDLE ue4ssReadyEvent);

	// Install the hooks (tries multiple patterns for reliability)
	// Returns true if at least one hook was successful
	bool Install();

	// Remove all hooks
	void Remove();

	// Check if engine has initialized
	bool IsEngineInitialized();

	// Fallback: called from WorldBeginPlay when Map_MainMenu fires before the
	// normal engine-init hooks have had a chance to run.  Safe to call even if
	// the engine is already initialized -- the internal once-guard will no-op it.
	void TriggerEngineReadyFallback(const wchar_t* reason);

	// Register a plugin callback to be notified when engine initializes
	void RegisterPluginCallback(PluginEngineInitCallback callback);

	// Unregister a plugin callback
	void UnregisterPluginCallback(PluginEngineInitCallback callback);

	// Legacy compatibility - set single callback (deprecated, use RegisterPluginCallback)
	void SetEngineInitCallback(void (*callback)());

	// Returns the trampoline address of the original FEngineLoop::Init, or 0 if not installed.
	// Cast to: int32_t(__fastcall*)(void* thisPtr)
	uintptr_t GetOriginalPtrEngineLoopInit();

	// Returns the trampoline address of the original UGameEngine::Init, or 0 if not installed.
	// Cast to: bool(__fastcall*)(void* thisPtr, void* InEngineLoop)
	uintptr_t GetOriginalPtrGameEngineInit();
}
