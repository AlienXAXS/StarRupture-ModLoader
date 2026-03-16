#pragma once

#include "plugin_interface.h"

namespace Compass
{
	// Install the AHUD::PostRender hook.  Returns false if no pattern was found.
	bool Install(IPluginScanner* scanner, IPluginHooks* hooks);

	// Remove the hook and clean up all state.
	void Remove(IPluginHooks* hooks);

	// Register the StaticLoadObject function pointer found via pattern scan.
	// Must be called before the first draw frame for textures to load without
	// requiring the player to open their map first.
	void SetStaticLoadObject(uintptr_t addr);
}
