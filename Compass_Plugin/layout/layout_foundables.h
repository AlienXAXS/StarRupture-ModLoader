#pragma once

#include <vector>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "../plugin_helpers.h"

// ---------------------------------------------------------------------------
// Foundable scanning
//
// Reads from ACrMapMenuDataReplicationHelper->FoundableMarkerDataContainer,
// which is replicated to all clients via the map menu subsystem.
//
// Types:
//   DeadBody (1) -- lootable dead body on the map
//   Drone    (2) -- lootable drone on the map
// ---------------------------------------------------------------------------

namespace Layout
{
	enum class FoundableType : int
	{
		DeadBody = 1,
		Drone = 2,
	};

	struct FoundableEntry
	{
		SDK::FVector location; // X,Y from FVector2f; Z set to 0
		FoundableType type;
	};

	inline std::vector<FoundableEntry> ScanFoundables(SDK::UWorld* world)
	{
		std::vector<FoundableEntry> result;
		if (!world)
			return result;

		auto* gameState = world->GameState
			                  ? static_cast<SDK::ACrGameStateBase*>(world->GameState)
			                  : nullptr;
		auto* mapHelper = gameState ? gameState->MapMenuDataReplicationHelper : nullptr;
		if (!mapHelper)
			return result;

		auto& data = mapHelper->FoundableMarkerDataContainer.FoundableMarkerData;
		LOG_TRACE("[ScanFoundables] mapHelper=%p data.Num()=%d", static_cast<void*>(mapHelper), data.Num());

		for (int i = 0; i < data.Num(); i++)
		{
			const auto& item = data[i];

			if (item.FoundableType == SDK::ECrFoundableType::Undefined ||
				item.FoundableType == SDK::ECrFoundableType::ECrFoundableType_MAX)
				continue;

			FoundableEntry e;
			e.location = {item.Location.X, item.Location.Y, 0.0f};
			e.type = static_cast<FoundableType>(static_cast<int>(item.FoundableType));

			LOG_TRACE("[ScanFoundables] [%d] type=%d loc=(%.0f,%.0f)",
			          i, static_cast<int>(e.type), e.location.X, e.location.Y);

			result.push_back(e);
		}

		return result;
	}
} // namespace Layout
