#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

namespace Layout
{
	// ---------------------------------------------------------------------------
	// POI type / state — mirrors ECrPointOfInterestType / EPointOfInterestState
	// ---------------------------------------------------------------------------

	enum class PoiType : int
	{
		Undefined = 0,
		Antena = 1,
		AbandonedBase = 2,
		Cave = 3,
		Obelisk = 4,
		OrbitalLander = 5,
		ForgottenEngine = 6,
	};

	enum class PoiState : int
	{
		Hidden = 0,
		VisibleUnknown = 1,
		VisibleDiscovered = 2,
		Disabled = 3,
	};

	// ---------------------------------------------------------------------------

	struct MarkerEntry
	{
		SDK::FVector location;
		PoiType type;
		PoiState state;
	};

	// ---------------------------------------------------------------------------

	// Returns all visible ACrPointOfInterestMarkerActor instances across all loaded levels.
	// Skips Hidden and Disabled markers — they are not shown on the player's map.
	// POI markers are typically spawned into streamed sub-levels, so we scan world->Levels
	// (which contains PersistentLevel + all currently loaded streaming levels) rather than
	// PersistentLevel alone.
	inline std::vector<MarkerEntry> ScanMarkers(SDK::UWorld* world)
	{
		std::vector<MarkerEntry> result;
		if (!world)
		{
			LOG_DEBUG("[ScanMarkers] world is null — aborting");
			return result;
		}

		SDK::TArray<SDK::AActor*> actors;
		SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ACrPointOfInterestMarkerActor::StaticClass(), &actors);
		LOG_TRACE("[ScanMarkers] GetAllActorsOfClass returned %d actors", actors.Num());

		int filteredState = 0;
		int filteredType = 0;

		for (int i = 0; i < actors.Num(); i++)
		{
			SDK::AActor* actor = actors[i];
			if (!actor)
				continue;

			auto* marker = static_cast<SDK::ACrPointOfInterestMarkerActor*>(actor);
			const auto state = static_cast<PoiState>(static_cast<int>(marker->GetPointOfInterestState()));
			const auto type = static_cast<PoiType>(static_cast<int>(marker->GetPointOfInterestType()));

			// Skip Hidden and Disabled markers — they are not shown on the player's map.
			// Exception: Antena markers are always shown on the map even when Hidden/Disabled, so we include them regardless of state.
			if ((state == PoiState::Hidden || state == PoiState::Disabled) && type != PoiType::Antena)
			{
				filteredState++;
				continue;
			}

			if (type == PoiType::OrbitalLander || type == PoiType::ForgottenEngine)
			{
				filteredType++;
				continue;
			}

			MarkerEntry e;
			e.location = marker->K2_GetActorLocation();
			e.type = type;
			e.state = state;

			LOG_TRACE("[ScanMarkers] accepted: type=%d state=%d loc=(%.0f,%.0f,%.0f)",
			          static_cast<int>(e.type), static_cast<int>(e.state),
			          e.location.X, e.location.Y, e.location.Z);

			result.push_back(e);
		}

		LOG_TRACE("[ScanMarkers] Done — total=%d filtered_state=%d filtered_type=%d kept=%d",
		          actors.Num(), filteredState, filteredType, static_cast<int>(result.size()));

		return result;
	}
} // namespace Layout
