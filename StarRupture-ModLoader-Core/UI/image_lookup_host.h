#pragma once

// The loader's engine side of the image lookups (image_lookup.h): the item
// list the asset registry gives and the icon of each item, and the object
// path of a live texture. Client builds only. Game thread only.

#include "UI/image_lookup.h"

namespace SDK { class UTexture2D; }

namespace ImageLookup
{
	// The loader's item source, for the pump.
	ItemSource& EngineItems();

	// The same for buildings: the building data assets and the icon of each.
	ItemSource& EngineBuildings();

	// The lookup behind IPluginImages::FromTexture.
	PluginImageId FromTexture(SDK::UTexture2D* texture);

	// An engine call made for the image library faulted and was caught. The engine's own locks
	// may be held after that, so no further call is made into it for images this session: the
	// lookups answer "cannot" and the pump reads nothing from the game.
	void NoteEngineFault();
	bool EngineFaulted();
}
