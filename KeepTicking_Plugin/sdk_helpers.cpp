#include "sdk_helpers.h"
#include "plugin_helpers.h"
#include "../StarRupture SDK/SDK/Engine_classes.hpp"

namespace SDKHelpers
{
	SDK::UWorld* GetWorld()
	{
		// Use the SDK's built-in UWorld::GetWorld() function
		// This is implemented in SDK/Engine_functions.cpp
		SDK::UWorld* World = SDK::UWorld::GetWorld();

		if (!World)
		{
			LOG_DEBUG("[SDK] UWorld::GetWorld() returned null");
			return nullptr;
		}

		return World;
	}

	SDK::UNetDriver* GetNetDriver(SDK::UWorld* World)
	{
		if (!World)
			return nullptr;

		// Access NetDriver directly from UWorld
		return World->NetDriver;
	}

	int32_t GetPlayerCount()
	{
		SDK::UWorld* World = GetWorld();
		if (!World)
		{
			LOG_DEBUG("[SDK] Cannot get player count - World is null");
			return -1;
		}

		// Try to get NetDriver
		SDK::UNetDriver* NetDriver = GetNetDriver(World);
		if (!NetDriver)
		{
			LOG_DEBUG("[SDK] Cannot get player count - NetDriver is null (might be listen server or single player)");
			return -1;
		}

		// Get client connections array
		// NetDriver->ClientConnections is TArray<UNetConnection*>
		int32_t playerCount = NetDriver->ClientConnections.Num();

		LOG_DEBUG("[SDK] Found %d connected clients via NetDriver", playerCount);
		return playerCount;
	}

	int32_t GetLocalPlayerCount(SDK::UWorld* World)
	{
		if (!World)
			return -1;

		// Access game instance
		SDK::UGameInstance* GameInstance = World->OwningGameInstance;
		if (!GameInstance)
		{
			LOG_DEBUG("[SDK] Cannot get local player count - GameInstance is null");
			return -1;
		}

		// Count local players
		int32_t localPlayerCount = GameInstance->LocalPlayers.Num();

		LOG_DEBUG("[SDK] Found %d local players via GameInstance", localPlayerCount);
		return localPlayerCount;
	}
}