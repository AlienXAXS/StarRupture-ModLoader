#pragma once

#include <vector>
#include "MassAIPrototypeEnemyRuntime_classes.hpp"
#include "../plugin_helpers.h"

// ---------------------------------------------------------------------------
// Enemy scanning
//
// Uses GetAllActorsOfClass with AMassEnemyCharacterBase -- the C++ base for
// every enemy type (Exploder, Melee, Ranged, etc.).  Only enemies within the
// current streaming radius exist as actors, which is exactly what we want
// for a nearby-threat compass display.
//
// Dead filter: Dissolve > 0 means the death sequence has started (replicated).
// ---------------------------------------------------------------------------

namespace Layout
{

struct EnemyEntry
{
	SDK::FVector location;
};

inline std::vector<EnemyEntry> ScanEnemies(SDK::UWorld* world)
{
	std::vector<EnemyEntry> result;
	if (!world)
		return result;

	SDK::TArray<SDK::AActor*> actors;
	SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::AMassEnemyCharacterBase::StaticClass(), &actors);
	LOG_TRACE("[ScanEnemies] GetAllActorsOfClass returned %d actors", actors.Num());

	for (int i = 0; i < actors.Num(); i++)
	{
		SDK::AActor* actor = actors[i];
		if (!actor)
			continue;

		auto* enemy = static_cast<SDK::AMassEnemyCharacterBase*>(actor);

		// Dissolve > 0 means the death/dissolve sequence has started -- skip
		if (enemy->Dissolve > 0.0f)
			continue;

		EnemyEntry e;
		e.location = enemy->K2_GetActorLocation();
		result.push_back(e);
	}

	LOG_TRACE("[ScanEnemies] kept %d live enemies", (int)result.size());
	return result;
}

} // namespace Layout
