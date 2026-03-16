#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

namespace Layout
{

struct PlayerEntry
{
	SDK::FVector location;
	std::string  name;
};

// Returns all players in the level, excluding localPawn.
inline std::vector<PlayerEntry> ScanPlayers(SDK::UWorld* world, SDK::APawn* localPawn)
{
	std::vector<PlayerEntry> result;
	if (!world || !world->PersistentLevel)
		return result;

	auto& actors = world->PersistentLevel->Actors;
	for (int i = 0; i < actors.Num(); i++)
	{
		SDK::AActor* actor = actors[i];
		if (!actor || actor == localPawn)
			continue;
		if (!actor->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
			continue;

		PlayerEntry e;
		e.location = actor->K2_GetActorLocation();

		auto* state = static_cast<SDK::APawn*>(actor)->PlayerState;
		if (state)
			e.name = state->PlayerNamePrivate.ToString();

		result.push_back(e);
	}
	return result;
}

} // namespace Layout
