#include "fake_player.h"
#include "plugin_helpers.h"
#include "sdk_helpers.h"
#include "../StarRupture SDK/SDK/Engine_classes.hpp"

#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


namespace Hooks::FakePlayer
{
	static long g_callCount = 0;
	static bool g_playerActive = false;
	static SDK::APlayerController* g_fakeController = nullptr;
	static SDK::APawn* g_fakePawn = nullptr;

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

		// Get the game mode to find the default pawn class
		SDK::AGameModeBase* gameMode = world->AuthorityGameMode;
		if (!gameMode)
		{
			LOG_ERROR("[FakePlayer] No game mode available");
			return;
		}

		LOG_DEBUG("[FakePlayer] Game mode: %s", gameMode->GetFullName().c_str());

		// Try to get the default pawn class
		SDK::UClass* pawnClass = gameMode->DefaultPawnClass;
		if (!pawnClass)
		{
			LOG_WARN("[FakePlayer] No default pawn class, using APawn");
			pawnClass = SDK::APawn::StaticClass();
		}

		LOG_DEBUG("[FakePlayer] Pawn class: %s", pawnClass->GetFullName().c_str());

		// Spawn transform at player location
		SDK::FTransform spawnTransform;
		memset(&spawnTransform, 0, sizeof(spawnTransform));

		// Set rotation from player camera rotation (Pitch: 0.08, Yaw: 317.66, Roll: 360.00)
		// Convert Euler angles to quaternion
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

		// Write quaternion into FTransform.Rotation (FQuat layout: X, Y, Z, W as doubles)
		auto* rotPtr = reinterpret_cast<double*>(&spawnTransform.Rotation);
		rotPtr[0] = qx;
		rotPtr[1] = qy;
		rotPtr[2] = qz;
		rotPtr[3] = qw; // W at offset 0x18 (index 3)

		// Set translation (X: -330880.36, Y: -42325.93, Z: 2519.88)
		// These coords are in the rear of the spawn ship
		auto* transPtr = reinterpret_cast<double*>(&spawnTransform.Translation);
		transPtr[0] = -330880.36;
		transPtr[1] = -42325.93;
		transPtr[2] = 2519.88;

		// Set scale to 1,1,1
		auto* scalePtr = reinterpret_cast<double*>(&spawnTransform.Scale3D);
		scalePtr[0] = 1.0;
		scalePtr[1] = 1.0;
		scalePtr[2] = 1.0;

		// Spawn a player controller
		SDK::UClass* controllerClass = SDK::APlayerController::StaticClass();

		LOG_DEBUG("[FakePlayer] Spawning player controller at player location...");
		g_fakeController = static_cast<SDK::APlayerController*>(
			SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world,
				controllerClass,
				spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr,
				SDK::ESpawnActorScaleMethod::MultiplyWithRoot
			)
			);

		if (!g_fakeController)
		{
			LOG_ERROR("[FakePlayer] Failed to spawn player controller");
			return;
		}

		SDK::UGameplayStatics::FinishSpawningActor(g_fakeController, spawnTransform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		LOG_INFO("[FakePlayer] Player controller spawned: %s", g_fakeController->GetFullName().c_str());

		// Make controller invulnerable and immobile
		g_fakeController->bCanBeDamaged = false;
		g_fakeController->SetActorEnableCollision(false);
		g_fakeController->SetActorTickEnabled(false);

		// Spawn the pawn
		LOG_DEBUG("[FakePlayer] Spawning pawn...");
		g_fakePawn = static_cast<SDK::APawn*>(
			SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world,
				pawnClass,
				spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr,  // Owner
				SDK::ESpawnActorScaleMethod::MultiplyWithRoot
			)
			);

		if (!g_fakePawn)
		{
			LOG_ERROR("[FakePlayer] Failed to spawn pawn");
			// Clean up controller
			if (g_fakeController)
			{
				g_fakeController->K2_DestroyActor();
				g_fakeController = nullptr;
			}
			return;
		}

		SDK::UGameplayStatics::FinishSpawningActor(g_fakePawn, spawnTransform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		LOG_INFO("[FakePlayer] Pawn spawned: %s", g_fakePawn->GetFullName().c_str());

		// Make pawn invulnerable and completely immobile
		g_fakePawn->bCanBeDamaged = false;
		g_fakePawn->SetActorEnableCollision(false);
		g_fakePawn->SetActorTickEnabled(false);

		// Disable physics on all components
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

		LOG_DEBUG("[FakePlayer] All physics and movement disabled on pawn");

		// Possess the pawn with the controller
		LOG_DEBUG("[FakePlayer] Possessing pawn...");
		g_fakeController->Possess(g_fakePawn);

		g_playerActive = true;
		InterlockedIncrement(&g_callCount);

		LOG_INFO("[FakePlayer] Fake player fully spawned and active!");
		LOG_INFO("  Controller: 0x%llX", reinterpret_cast<uintptr_t>(g_fakeController));
		LOG_INFO("  Pawn: 0x%llX", reinterpret_cast<uintptr_t>(g_fakePawn));
	}

	void DespawnFakePlayer()
	{
		if (!g_playerActive)
		{
			LOG_DEBUG("[FakePlayer] No fake player to despawn");
			return;
		}

		LOG_INFO("[FakePlayer] Despawning fake player...");

		// Unpossess first
		if (g_fakeController && g_fakePawn)
		{
			g_fakeController->UnPossess();
		}

		// Destroy pawn
		if (g_fakePawn)
		{
			g_fakePawn->K2_DestroyActor();
			g_fakePawn = nullptr;
		}

		// Destroy controller
		if (g_fakeController)
		{
			g_fakeController->K2_DestroyActor();
			g_fakeController = nullptr;
		}

		g_playerActive = false;
		LOG_INFO("[FakePlayer] Fake player despawned");
	}

	bool Install()
	{
		LOG_INFO("FakePlayer: Spawn/despawn system ready");
		LOG_INFO("  Controlled by config: Hooks.PreventServerSleep");
		LOG_INFO("  Default: disabled (prevents server sleep only when explicitly enabled)");

		// We don't hook anything, just provide spawn/despawn functions
		return true;
	}

	void Remove()
	{
		DespawnFakePlayer();
	}

	long GetCallCount()
	{
		return g_callCount;
	}

	bool IsPlayerActive()
	{
		return g_playerActive;
	}
}
