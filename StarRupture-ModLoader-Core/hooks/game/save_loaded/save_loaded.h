#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// UCrMassSaveSubsystem::OnSaveLoaded Hook
//
// Purpose: Save-loaded signal - fires once the save has finished loading
//      and (hopefully) all actors have been spawned.
//
// Hook point:
//   UCrMassSaveSubsystem::execOnSaveLoaded -- the UHT exec thunk, which is what
//   UFunction::ExecFunction points at and how the engine actually reaches
//   OnSaveLoaded (bound as a dynamic delegate, entered via ProcessEvent).
//   Falls back to a pattern scan for the native
//   UCrMassSaveSubsystem::OnSaveLoaded(UCrMassSaveSubsystem* this).
//   See save_loaded.cpp for why the detour uses the three-argument exec shape
//   for both.
// ---------------------------------------------------------------------------

namespace Hooks::SaveLoaded
{
	// Callback signature for plugins
	using PluginSaveLoadedCallback = void(*)();

	// Install the hook
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Register a plugin callback to be notified when a save finishes loading
	void RegisterPluginCallback(PluginSaveLoadedCallback callback);

	// Unregister a plugin callback
	void UnregisterPluginCallback(PluginSaveLoadedCallback callback);

	// Returns the trampoline address of the original UCrMassSaveSubsystem::OnSaveLoaded, or 0 if not yet installed.
	// Cast to: void(__fastcall*)(void* context, void* stack, void* result)
	// -- the FNativeFuncPtr shape. Calling it with only `this` faults inside the
	// exec thunk's P_FINISH, which writes through the FFrame& in RDX.
	uintptr_t GetOriginalPtr();
}
