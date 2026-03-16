#pragma once

#include <vector>
#include <string>
#include "layout_markers.h"

// ---------------------------------------------------------------------------
// Cave scanning — delegates to ScanMarkers filtered to PoiType::Cave.
// Caves are ACrPointOfInterestMarkerActor with type = Cave (3).
// ---------------------------------------------------------------------------

namespace Layout
{

struct CaveEntry
{
	SDK::FVector location;
	std::string  name;
};

inline std::vector<CaveEntry> ScanCaves(SDK::UWorld* world)
{
	std::vector<CaveEntry> result;
	for (const auto& m : ScanMarkers(world))
	{
		if (m.type != PoiType::Cave)
			continue;
		result.push_back({ m.location });
	}
	return result;
}

} // namespace Layout
