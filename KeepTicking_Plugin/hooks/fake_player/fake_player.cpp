#include "fake_player.h"
#include "plugin_helpers.h"
#include "sdk_helpers.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


namespace Hooks::FakePlayer
{
	static long g_callCount = 0;
	static bool g_playerActive = false;
	static SDK::ACrPlayerControllerBase* g_fakeController = nullptr;
	static SDK::APawn* g_fakePawn = nullptr;
	static bool g_debugVisibleMode = false;

	void SpawnFakePlayer()
	{
		if (g_playerActive)
		{
			LOG_DEBUG("[FakePlayer] Fake player already spawned");
			return;
		}

		SDK::UWorld* world = SDK::UWorld::GetWorld();
		if (!world)
		{
			LOG_ERROR("[FakePlayer] Cannot spawn - world is null");
			return;
		}

		LOG_INFO("[FakePlayer] Attempting to spawn fake player...");

		SDK::AGameModeBase* gameMode = world->AuthorityGameMode;
		if (!gameMode)
		{
			LOG_ERROR("[FakePlayer] No game mode available");
			return;
		}

		LOG_DEBUG("[FakePlayer] Game mode: %s", gameMode->GetFullName().c_str());

		SDK::UClass* pawnClass = gameMode->DefaultPawnClass;
		if (!pawnClass)
		{
			LOG_WARN("[FakePlayer] No default pawn class, using APawn");
			pawnClass = SDK::APawn::StaticClass();
		}

		LOG_DEBUG("[FakePlayer] Pawn class: %s", pawnClass->GetFullName().c_str());

		// FIXED: Use brace initialization instead of memset
		SDK::FTransform spawnTransform{};

		// Set rotation
		double pitch = 0.08 * (M_PI / 180.0);
		double yaw = 317.66 * (M_PI / 180.0);
		double roll = 360.00 * (M_PI / 180.0);

		double cy = cos(yaw * 0.5);
		double sy = sin(yaw * 0.5);
		double cp = cos(pitch * 0.5);
		double sp = sin(pitch * 0.5);
		double cr = cos(roll * 0.5);
		double sr = sin(roll * 0.5);

		double qx = sr * cp * cy - cr * sp * sy;
		double qy = cr * sp * cy + sr * cp * sy;
		double qz = cr * cp * sy - sr * sp * cy;
		double qw = cr * cp * cy + sr * sp * sy;

		auto* rotPtr = reinterpret_cast<double*>(&spawnTransform.Rotation);
		rotPtr[0] = qx;
		rotPtr[1] = qy;
		rotPtr[2] = qz;
		rotPtr[3] = qw;

		auto* transPtr = reinterpret_cast<double*>(&spawnTransform.Translation);
		transPtr[0] = -330880.36;
		transPtr[1] = -42325.93;
		transPtr[2] = 2519.88;

		auto* scalePtr = reinterpret_cast<double*>(&spawnTransform.Scale3D);
		scalePtr[0] = 1.0;
		scalePtr[1] = 1.0;
		scalePtr[2] = 1.0;

		SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();

		LOG_DEBUG("[FakePlayer] Spawning controller...");
		g_fakeController = static_cast<SDK::ACrPlayerControllerBase*>(
			SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world, controllerClass, spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot
			)
		);

		if (!g_fakeController)
		{
			LOG_ERROR("[FakePlayer] Failed to spawn controller");
			return;
		}

		SDK::UGameplayStatics::FinishSpawningActor(g_fakeController, spawnTransform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		
		LOG_INFO("[FakePlayer] Controller spawned: %s", g_fakeController->GetFullName().c_str());

		if (g_fakeController->PlayerState)
		{
			LOG_INFO("[FakePlayer] PlayerState created: %s", g_fakeController->PlayerState->GetFullName().c_str());
		}
		else
		{
			LOG_WARN("[FakePlayer] PlayerState is NULL!");
		}
	
		g_fakeController->bCanBeDamaged = false;
		
		if (!g_debugVisibleMode)
		{
			g_fakeController->SetActorEnableCollision(false);
			g_fakeController->SetActorTickEnabled(false);
		}

		LOG_DEBUG("[FakePlayer] Spawning pawn...");
		g_fakePawn = static_cast<SDK::APawn*>(
			SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world, pawnClass, spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot
			)
		);

		if (!g_fakePawn)
		{
			LOG_ERROR("[FakePlayer] Failed to spawn pawn");
			g_fakeController = nullptr; // Don't destroy during shutdown
			return;
		}

		SDK::UGameplayStatics::FinishSpawningActor(g_fakePawn, spawnTransform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		
		LOG_INFO("[FakePlayer] Pawn spawned: %s", g_fakePawn->GetFullName().c_str());

		g_fakePawn->bCanBeDamaged = false;
		
		if (!g_debugVisibleMode)
		{
			g_fakePawn->SetActorEnableCollision(false);
			g_fakePawn->SetActorTickEnabled(false);

			SDK::TArray<SDK::UActorComponent*> components = g_fakePawn->K2_GetComponentsByClass(SDK::UPrimitiveComponent::StaticClass());
			for (int i = 0; i < components.Num(); i++)
			{
				SDK::UPrimitiveComponent* primComp = static_cast<SDK::UPrimitiveComponent*>(components[i]);
				if (primComp)
				{
					primComp->SetSimulatePhysics(false);
					primComp->SetEnableGravity(false);
					primComp->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
				}
			}
		}

		LOG_DEBUG("[FakePlayer] Possessing pawn...");
		g_fakeController->Possess(g_fakePawn);

		g_playerActive = true;
		InterlockedIncrement(&g_callCount);

		LOG_INFO("[FakePlayer] Fake player active!");
	}

	void DespawnFakePlayer()
	{
		if (!g_playerActive)
		{
			LOG_DEBUG("[FakePlayer] No fake player to despawn");
			return;
		}

		LOG_INFO("[FakePlayer] Despawning fake player...");

		// CRITICAL FIX: Just clear pointers during shutdown
		// The engine will clean up actors automatically
		// Calling K2_DestroyActor during shutdown causes heap corruption
		g_fakePawn = nullptr;
		g_fakeController = nullptr;
		g_playerActive = false;
		
		LOG_INFO("[FakePlayer] Fake player cleared (engine will cleanup)");
	}

	bool Install()
	{
		LOG_INFO("FakePlayer: Spawn/despawn system ready");
		return true;
	}

	void Remove()
	{
		// CRITICAL FIX: During shutdown, just clear pointers
		// Don't call DespawnFakePlayer() which might try to destroy actors
		if (g_playerActive)
		{
			LOG_INFO("[FakePlayer] Shutdown: clearing fake player pointers");
			g_fakePawn = nullptr;
			g_fakeController = nullptr;
			g_playerActive = false;
		}
	}

	long GetCallCount()
	{
		return g_callCount;
	}

	bool IsPlayerActive()
	{
		return g_playerActive;
	}

	void SetDebugVisibleMode(bool enabled)
	{
		g_debugVisibleMode = enabled;
		LOG_INFO("[FakePlayer] Debug visible mode %s", enabled ? "ENABLED" : "DISABLED");
	}
}
