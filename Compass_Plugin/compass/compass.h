#pragma once

#include "plugin_interface.h"

namespace Compass
{
	// Register the AHUD::PostRender callback via the modloader HUD hook interface.
	// Returns false if the HUD interface is unavailable (server build) or registration fails.
	bool Install(IPluginHooks* hooks);

	// Remove the hook and clean up all state.
	void Remove(IPluginHooks* hooks);

	// Register the StaticLoadObject function pointer found via pattern scan.
	// Must be called before the first draw frame for textures to load without
	// requiring the player to open their map first.
	void SetStaticLoadObject(uintptr_t addr);
}
