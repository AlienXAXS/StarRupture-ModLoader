#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include <algorithm>

// ---------------------------------------------------------------------------
// Dead body scanning
//
// All lootable corpse actors (ABP_Foundable_DeadBody_*, ABP_Foundable_OutsDeadBody_*,
// ABP_LootableCorpse*) inherit from ACrStorageAsBuilding.  We use ACrStorageAsBuilding
// as the IsA gate and then filter by class name to exclude generic storage buildings.
//
// Class name substrings that identify body actors (case-insensitive match):
//   "DeadBody", "Corpse", "LootableCorpse"
// ---------------------------------------------------------------------------

namespace Layout
{

struct BodyEntry
{
	SDK::FVector location;
};

namespace Detail
{
	inline bool IsBodyClassName(const std::string& name)
	{
		// Case-insensitive search for known dead-body class name fragments
		auto ci = [&](const char* sub) -> bool {
			auto it = std::search(
				name.begin(), name.end(),
				sub, sub + strlen(sub),
				[](char a, char b) { return std::tolower(a) == std::tolower(b); });
			return it != name.end();
		};
		return ci("DeadBody") || ci("Corpse") || ci("LootableCorpse");
	}
}

// Returns all lootable corpse actors in the world.
inline std::vector<BodyEntry> ScanBodies(SDK::UWorld* world)
{
	std::vector<BodyEntry> result;
	if (!world)
		return result;

	SDK::TArray<SDK::AActor*> actors;
	SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ACrStorageAsBuilding::StaticClass(), &actors);

	for (int i = 0; i < actors.Num(); i++)
	{
		SDK::AActor* actor = actors[i];
		if (!actor)
			continue;
		if (!Detail::IsBodyClassName(actor->GetName()))
			continue;

		BodyEntry e;
		e.location = actor->K2_GetActorLocation();
		result.push_back(e);
	}
	return result;
}

} // namespace Layout
