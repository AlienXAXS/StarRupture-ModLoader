#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "../plugin_helpers.h"

namespace Layout
{
	struct PlayerMarkerEntry
	{
		SDK::FVector location; // X,Y from FVector2f; Z set to 0
		std::wstring playerName;
		SDK::FLinearColor color;
		float rotationZ;
		SDK::uint8 flags; // EPlayerMarkerFlags
	};

	// Returns all player-placed map markers replicated via ACrMapMenuDataReplicationHelper.
	// Must be called after ScanBaseCores so the helper is guaranteed non-null in GameState
	// (either the real replicated actor or the local stand-in we spawn on first scan).
	inline std::vector<PlayerMarkerEntry> ScanPlayerMarkers(SDK::UWorld* world)
	{
		std::vector<PlayerMarkerEntry> result;
		if (!world)
			return result;

		auto* gameState = world->GameState
			                  ? static_cast<SDK::ACrGameStateBase*>(world->GameState)
			                  : nullptr;
		auto* mapHelper = gameState ? gameState->MapMenuDataReplicationHelper : nullptr;
		if (!mapHelper)
			return result;

		// Resolve local player name so we can skip our own dot.
		std::wstring localName;
		SDK::APlayerController* localPC = SDK::UGameplayStatics::GetPlayerController(world, 0);
		if (localPC && localPC->PlayerState)
		{
			const wchar_t* lp = localPC->PlayerState->PlayerNamePrivate.CStr();
			if (lp && localPC->PlayerState->PlayerNamePrivate.Num() > 0)
				localName = lp;
		}

		auto& data = mapHelper->PlayersMarkerDataContainer.PlayersMarkerData;
		LOG_TRACE("[ScanPlayerMarkers] mapHelper=%p data.Num()=%d localName='%ls'",
		          static_cast<void*>(mapHelper), data.Num(), localName.c_str());

		for (int i = 0; i < data.Num(); i++)
		{
			const auto& item = data[i];

			PlayerMarkerEntry e;
			const wchar_t* p = item.Player.CStr();
			e.playerName = (p && item.Player.Num() > 0) ? p : L"";

			// Skip the local player - no need to show yourself.
			if (!localName.empty() && e.playerName == localName)
				continue;

			e.location = {item.Location.X, item.Location.Y, 0.0f};
			e.color = item.PlayerColor;
			e.rotationZ = item.RotationZ;
			e.flags = static_cast<SDK::uint8>(item.PlayerMarkerFlags);

			LOG_TRACE("[ScanPlayerMarkers] [%d] player='%ls' loc=(%.0f,%.0f) flags=%d",
			          i, e.playerName.c_str(), e.location.X, e.location.Y, static_cast<int>(e.flags));

			result.push_back(e);
		}

		return result;
	}
} // namespace Layout
