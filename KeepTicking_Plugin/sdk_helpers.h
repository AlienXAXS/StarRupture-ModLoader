#pragma once

#include "../StarRupture SDK/Server/SDK/Engine_classes.hpp"

namespace SDKHelpers
{
	// Get the current UWorld instance using SDK's native GetWorld() function
	SDK::UWorld* GetWorld();

	// Get the number of connected players (works on dedicated servers)
	// Returns -1 if unable to determine
	int32_t GetPlayerCount();

	// Get NetDriver from world (for server player count)
	SDK::UNetDriver* GetNetDriver(SDK::UWorld* World);

	// Get local player count (for listen servers/clients)
	int32_t GetLocalPlayerCount(SDK::UWorld* World);
}
