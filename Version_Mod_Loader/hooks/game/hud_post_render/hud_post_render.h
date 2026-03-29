#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// AHUD::PostRender Hook  (client builds only)
//
// Purpose: Fires every frame after the engine has drawn the base HUD.
//          Plugins can register callbacks to draw additional HUD overlays
//          using the UCanvas* available on the AHUD object.
//
// Hook point:
//   void AHUD::PostRender()
//
// Callback receives the raw AHUD* pointer (as void*) — plugins cast to
// SDK::AHUD* to access hud->Canvas, hud->GetOwningPlayerController(), etc.
//
// The modloader calls the original AHUD::PostRender BEFORE notifying plugins
// so the base HUD is always fully rendered first.
//
// Also resolves UCrMapManuSubsystem::GatherPlayersData at Install time and
// exposes the cached address via GetGatherPlayersDataAddress().
// ---------------------------------------------------------------------------

namespace Hooks::HUDPostRender
{
	// Callback signature for plugins.
	// Receives the raw AHUD* pointer (as void*) — plugins cast to SDK::AHUD*.
	using PluginHUDPostRenderCallback = void(*)(void* hud);

	// Install the hook.  Returns true on success.
	// Called during modloader client startup.
	bool Install();

	// Remove the hook and clear all registered callbacks.
	void Remove();

	// Returns true if the hook is currently installed.
	bool IsInstalled();

	// Register a plugin callback to be notified each frame after PostRender.
	// Installs the hook lazily on the first registration if not already done.
	void RegisterPluginCallback(PluginHUDPostRenderCallback callback);

	// Unregister a previously registered callback.
	void UnregisterPluginCallback(PluginHUDPostRenderCallback callback);

	// Returns the resolved address of UCrMapManuSubsystem::GatherPlayersData,
	// or 0 if the pattern was not found during Install().
	uintptr_t GetGatherPlayersDataAddress();
}

#endif // MODLOADER_CLIENT_BUILD
