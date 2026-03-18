#pragma once

#include <vector>
#include <string>
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "../plugin_helpers.h"

namespace Layout
{

struct CustomPinEntry
{
	SDK::FVector      location;     // X,Y from FVector2f; Z set to 0
	std::wstring      playerName;
	SDK::FLinearColor color;
};

// Returns all personal map markers from ACrGameStateBase::PlayerPersonalMarkers.
// These are custom pins placed by any player -- replicated to all clients via GameState.
inline std::vector<CustomPinEntry> ScanCustomPins(SDK::UWorld* world)
{
	std::vector<CustomPinEntry> result;
	if (!world)
		return result;

	auto* gameState = world->GameState
		? static_cast<SDK::ACrGameStateBase*>(world->GameState)
		: nullptr;
	if (!gameState)
		return result;

	auto& pins = gameState->PlayerPersonalMarkers;
	LOG_TRACE("[ScanCustomPins] gameState=%p pins.Num()=%d", (void*)gameState, pins.Num());

	for (int i = 0; i < pins.Num(); i++)
	{
		const auto& item = pins[i];

		CustomPinEntry e;
		e.location = { item.MarkerPosition.X, item.MarkerPosition.Y, 0.0f };
		const wchar_t* p = item.PlayerName.CStr();
		e.playerName = (p && item.PlayerName.Num() > 0)
			? (std::wstring(p) + L"'s Marker")
			: L"'s Marker";
		e.color = item.PlayerColor;

		LOG_TRACE("[ScanCustomPins] [%d] player='%ls' loc=(%.0f,%.0f)",
			i, e.playerName.c_str(), e.location.X, e.location.Y);

		result.push_back(e);
	}

	return result;
}

} // namespace Layout
