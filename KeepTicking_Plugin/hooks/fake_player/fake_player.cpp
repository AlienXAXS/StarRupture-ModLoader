#include "fake_player.h"
#include "plugin_helpers.h"
#include "sdk_helpers.h"
#include "plugin_config.h"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "MassAIPrototypeEnemyRuntime_classes.hpp"
#include "Chimera_functions.cpp"

#define _USE_MATH_DEFINES
#include <cmath>
#include <vector>
#include <algorithm>
#include <cfloat>

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

	// UWorld::RemoveController - removes controller from PlayerControllerList
	using UWorld_RemoveController_t = void(__fastcall*)(SDK::UWorld* world, SDK::AController* controller);
	static UWorld_RemoveController_t g_removeControllerFn = nullptr;

	// --- Map traversal state ---
	static bool g_traversing = false;
	static int g_waypointIndex = 0;

	struct Waypoint
	{
		double x, y, z;
	};

	// Helper to safely get actor location - isolated for SEH compatibility
	static bool SafeGetActorLocation(SDK::AActor* actor, SDK::FVector& outLoc)
	{
		__try
		{
			outLoc = actor->K2_GetActorLocation();
			return true;
		}
		__except (1)
		{
			return false;
		}
	}

	// Helper to safely teleport pawn - isolated for SEH compatibility
	static bool SafeSetActorLocation(SDK::APawn* pawn, const SDK::FVector& loc)
	{
		__try
		{
			return pawn->K2_SetActorLocation(loc, false, nullptr, false);
		}
		__except (1)
		{
			return false;
		}
	}

	// Build a grid of waypoints by scanning all world actors for their locations.
	// This dynamically determines the map bounds instead of using hardcoded values.
	static std::vector<Waypoint> BuildWaypointGridFromWorldActors()
	{
		std::vector<Waypoint> pts;

		LOG_DEBUG("[FakePlayer] BuildWaypointGridFromWorldActors() - enter");

		SDK::UWorld* world = SDK::UWorld::GetWorld();
		if (!world)
		{
			LOG_ERROR("[FakePlayer] Cannot build waypoints - world is null");
			return pts;
		}

		LOG_DEBUG("[FakePlayer] World pointer: %p", world);

		// Gather all actors in the world
		LOG_DEBUG("[FakePlayer] Calling GetAllActorsOfClass...");
		SDK::TArray<SDK::AActor*> allActors;
		SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::AActor::StaticClass(), &allActors);

		int actorCount = allActors.Num();
		LOG_DEBUG("[FakePlayer] GetAllActorsOfClass returned %d actors", actorCount);

		if (actorCount == 0)
		{
			LOG_WARN("[FakePlayer] No actors found in world, using fallback bounds");
			// Fallback to conservative defaults
			constexpr double minX = -450000.0;
			constexpr double maxX = -200000.0;
			constexpr double minY = -150000.0;
			constexpr double maxY = 50000.0;
			constexpr double z = 3000.0;
			constexpr double step = 25000.0;

			bool reverseY = false;
			for (double x = minX; x <= maxX; x += step)
			{
				if (!reverseY)
					for (double y = minY; y <= maxY; y += step)
						pts.push_back({x, y, z});
				else
					for (double y = maxY; y >= minY; y -= step)
						pts.push_back({x, y, z});
				reverseY = !reverseY;
			}
			LOG_DEBUG("[FakePlayer] Fallback generated %zu waypoints", pts.size());
			return pts;
		}

		// Compute AABB from all actor positions
		double minX = DBL_MAX, maxX = -DBL_MAX;
		double minY = DBL_MAX, maxY = -DBL_MAX;
		double minZ = DBL_MAX, maxZ = -DBL_MAX;
		int validActors = 0;
		int skippedNull = 0;
		int skippedOrigin = 0;
		int skippedError = 0;

		for (int i = 0; i < actorCount; i++)
		{
			SDK::AActor* actor = allActors[i];
			if (!actor)
			{
				skippedNull++;
				continue;
			}

			SDK::FVector loc{};
			if (!SafeGetActorLocation(actor, loc))
			{
				skippedError++;
				continue;
			}

			// Skip actors at origin (0,0,0) as they're likely defaults/unplaced
			if (loc.X == 0.0 && loc.Y == 0.0 && loc.Z == 0.0)
			{
				skippedOrigin++;
				continue;
			}

			minX = (std::min)(minX, loc.X);
			maxX = (std::max)(maxX, loc.X);
			minY = (std::min)(minY, loc.Y);
			maxY = (std::max)(maxY, loc.Y);
			minZ = (std::min)(minZ, loc.Z);
			maxZ = (std::max)(maxZ, loc.Z);
			validActors++;
		}

		LOG_DEBUG("[FakePlayer] Actor scan: %d valid, %d null, %d at origin, %d errored (of %d total)",
		          validActors, skippedNull, skippedOrigin, skippedError, actorCount);

		if (validActors == 0)
		{
			LOG_WARN("[FakePlayer] No valid actor positions found, using fallback bounds");
			constexpr double step = 25000.0;
			constexpr double z = 3000.0;
			double fbMinX = -450000.0, fbMaxX = -200000.0;
			double fbMinY = -150000.0, fbMaxY = 50000.0;

			bool reverseY = false;
			for (double x = fbMinX; x <= fbMaxX; x += step)
			{
				if (!reverseY)
					for (double y = fbMinY; y <= fbMaxY; y += step)
						pts.push_back({x, y, z});
				else
					for (double y = fbMaxY; y >= fbMinY; y -= step)
						pts.push_back({x, y, z});
				reverseY = !reverseY;
			}
			LOG_DEBUG("[FakePlayer] Fallback generated %zu waypoints", pts.size());
			return pts;
		}

		// Add some padding around the bounds (10% of each axis range)
		double padX = (maxX - minX) * 0.10;
		double padY = (maxY - minY) * 0.10;
		minX -= padX;
		maxX += padX;
		minY -= padY;
		maxY += padY;

		// Use a Z altitude slightly above the average actor Z, or a safe default
		double avgZ = (minZ + maxZ) * 0.5;
		double traversalZ = (std::max)(avgZ + 1000.0, 3000.0); // At least 3000 units up

		// Grid step - aim for ~25000 units but adapt to map size
		constexpr double preferredStep = 25000.0;
		double rangeX = maxX - minX;
		double rangeY = maxY - minY;
		double step = preferredStep;

		// Clamp step so we don't get an absurd number of waypoints
		// but also ensure we have at least a few waypoints per axis
		if (rangeX / step > 100.0 || rangeY / step > 100.0)
			step = (std::max)(rangeX, rangeY) / 100.0;
		if (step < 1000.0)
			step = 1000.0;

		LOG_DEBUG("[FakePlayer] World bounds from %d actors: X[%.0f, %.0f] Y[%.0f, %.0f] Z[%.0f, %.0f]",
		          validActors, minX, maxX, minY, maxY, minZ, maxZ);
		LOG_DEBUG("[FakePlayer] Traversal grid: step=%.0f, Z=%.0f", step, traversalZ);

		// Build snake-pattern waypoints
		bool reverseY = false;
		for (double x = minX; x <= maxX; x += step)
		{
			if (!reverseY)
			{
				for (double y = minY; y <= maxY; y += step)
					pts.push_back({x, y, traversalZ});
			}
			else
			{
				for (double y = maxY; y >= minY; y -= step)
					pts.push_back({x, y, traversalZ});
			}
			reverseY = !reverseY;
		}

		LOG_DEBUG("[FakePlayer] Generated %zu waypoints from world actor bounds", pts.size());
		return pts;
	}

	static std::vector<Waypoint> g_waypoints;

	// ---

	void SpawnFakePlayer()
	{
		LOG_INFO("[FakePlayer] === SpawnFakePlayer() ENTER ===");

		if (g_playerActive)
		{
			LOG_DEBUG("[FakePlayer] Fake player already spawned");
			return;
		}

		LOG_DEBUG("[FakePlayer] Calling SDK::UWorld::GetWorld()...");
		SDK::UWorld* world = SDK::UWorld::GetWorld();
		if (!world)
		{
			LOG_ERROR("[FakePlayer] Cannot spawn - world is null");
			return;
		}
		LOG_DEBUG("[FakePlayer] World: %p", world);

		LOG_INFO("[FakePlayer] Attempting to spawn fake player...");

		LOG_DEBUG("[FakePlayer] Getting AuthorityGameMode...");
		SDK::AGameModeBase* gameMode = world->AuthorityGameMode;
		if (!gameMode)
		{
			LOG_ERROR("[FakePlayer] No game mode available");
			return;
		}

		LOG_DEBUG("[FakePlayer] Game mode ptr: %p", gameMode);
		LOG_DEBUG("[FakePlayer] Game mode: %s", gameMode->GetFullName().c_str());

		LOG_DEBUG("[FakePlayer] Getting DefaultPawnClass...");
		SDK::UClass* pawnClass = gameMode->DefaultPawnClass;
		if (!pawnClass)
		{
			LOG_WARN("[FakePlayer] No default pawn class, using APawn");
			pawnClass = SDK::APawn::StaticClass();
		}

		LOG_DEBUG("[FakePlayer] Pawn class ptr: %p", pawnClass);
		LOG_DEBUG("[FakePlayer] Pawn class: %s", pawnClass->GetFullName().c_str());

		// FIXED: Use brace initialization instead of memset
		LOG_DEBUG("[FakePlayer] Initializing FTransform...");
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
		transPtr[0] = -320766.63;
		transPtr[1] = -57072.67;
		transPtr[2] = 1991.07;

		auto* scalePtr = reinterpret_cast<double*>(&spawnTransform.Scale3D);
		scalePtr[0] = 1.0;
		scalePtr[1] = 1.0;
		scalePtr[2] = 1.0;

		LOG_DEBUG("[FakePlayer] Transform set: pos=(%.2f, %.2f, %.2f) quat=(%.4f, %.4f, %.4f, %.4f)",
		          transPtr[0], transPtr[1], transPtr[2], qx, qy, qz, qw);

		LOG_DEBUG("[FakePlayer] Getting ACrPlayerControllerBase::StaticClass()...");
		SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();
		if (!controllerClass)
		{
			LOG_ERROR("[FakePlayer] ACrPlayerControllerBase::StaticClass() returned null!");
			return;
		}
		LOG_DEBUG("[FakePlayer] Controller class ptr: %p", controllerClass);

		LOG_DEBUG("[FakePlayer] Spawning controller via BeginDeferredActorSpawnFromClass...");
		g_fakeController = static_cast<SDK::ACrPlayerControllerBase*>(
			SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world, controllerClass, spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot
			)
		);

		if (!g_fakeController)
		{
			LOG_ERROR("[FakePlayer] Failed to spawn controller (BeginDeferredActorSpawnFromClass returned null)");
			return;
		}
		LOG_DEBUG("[FakePlayer] Controller deferred spawn OK: %p", g_fakeController);

		LOG_DEBUG("[FakePlayer] Calling FinishSpawningActor on controller...");
		SDK::UGameplayStatics::FinishSpawningActor(g_fakeController, spawnTransform,
		                                           SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		LOG_DEBUG("[FakePlayer] Controller FinishSpawningActor completed");

		LOG_INFO("[FakePlayer] Controller spawned: %s", g_fakeController->GetFullName().c_str());

		if (g_fakeController->PlayerState)
		{
			LOG_INFO("[FakePlayer] PlayerState created: %s", g_fakeController->PlayerState->GetFullName().c_str());
		}
		else
		{
			LOG_WARN("[FakePlayer] PlayerState is NULL!");
		}

		LOG_DEBUG("[FakePlayer] Setting controller bCanBeDamaged = false");
		g_fakeController->bCanBeDamaged = false;

		if (!g_debugVisibleMode)
		{
			LOG_DEBUG("[FakePlayer] Setting controller collision OFF, tick OFF");
			g_fakeController->SetActorEnableCollision(false);
			g_fakeController->SetActorTickEnabled(false);
		}

		LOG_DEBUG("[FakePlayer] Spawning pawn via BeginDeferredActorSpawnFromClass...");
		g_fakePawn = static_cast<SDK::APawn*>(
			SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
				world, pawnClass, spawnTransform,
				SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
				nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot
			)
		);

		if (!g_fakePawn)
		{
			LOG_ERROR("[FakePlayer] Failed to spawn pawn (BeginDeferredActorSpawnFromClass returned null)");
			g_fakeController = nullptr; // Don't destroy during shutdown
			return;
		}
		LOG_DEBUG("[FakePlayer] Pawn deferred spawn OK: %p", g_fakePawn);

		LOG_DEBUG("[FakePlayer] Setting pawn replication properties...");
		g_fakePawn->bAlwaysRelevant = true;
		g_fakePawn->NetCullDistanceSquared = 1e+15f;
		g_fakeController->bAlwaysRelevant = true;

		LOG_DEBUG("[FakePlayer] Calling FinishSpawningActor on pawn...");
		SDK::UGameplayStatics::FinishSpawningActor(g_fakePawn, spawnTransform,
		                                           SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
		LOG_DEBUG("[FakePlayer] Pawn FinishSpawningActor completed");

		LOG_INFO("[FakePlayer] Pawn spawned: %s", g_fakePawn->GetFullName().c_str());

		LOG_DEBUG("[FakePlayer] Setting pawn bCanBeDamaged = false");
		g_fakePawn->bCanBeDamaged = false;

		if (!g_debugVisibleMode)
		{
			LOG_DEBUG("[FakePlayer] Setting pawn collision OFF, tick OFF");
			g_fakePawn->SetActorEnableCollision(false);
			g_fakePawn->SetActorTickEnabled(false);

			LOG_DEBUG("[FakePlayer] Getting pawn components to disable physics/collision...");
			SDK::TArray<SDK::UActorComponent*> components = g_fakePawn->K2_GetComponentsByClass(
				SDK::UPrimitiveComponent::StaticClass());
			LOG_DEBUG("[FakePlayer] Pawn has %d primitive components", components.Num());
			for (int i = 0; i < components.Num(); i++)
			{
				auto primComp = static_cast<SDK::UPrimitiveComponent*>(components[i]);
				if (primComp)
				{
					primComp->SetSimulatePhysics(false);
					primComp->SetEnableGravity(false);
					primComp->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
					primComp->SetIsReplicated(false);
				}
			}
			LOG_DEBUG("[FakePlayer] All primitive components configured");
		}

		LOG_DEBUG("[FakePlayer] Calling Possess()...");
		g_fakeController->Possess(g_fakePawn);
		LOG_DEBUG("[FakePlayer] Possess() completed");

		g_playerActive = true;
		InterlockedIncrement(&g_callCount);

		LOG_INFO("[FakePlayer] === SpawnFakePlayer() COMPLETE - fake player active! ===");
	}

	void DespawnFakePlayer()
	{
		if (!g_playerActive)
		{
			LOG_DEBUG("[FakePlayer] No fake player to despawn");
			return;
		}

		LOG_INFO("[FakePlayer] Despawning fake player...");

		// Stop traversal if running
		g_traversing = false;

		// Get the world and game mode for proper logout
		SDK::UWorld* world = SDK::UWorld::GetWorld();
		SDK::AGameModeBase* gameMode = world ? world->AuthorityGameMode : nullptr;

		// Step 2b: Remove from PlayerArray
		if (g_fakeController && g_fakeController->PlayerState)
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			if (world && world->GameState)
			{
				auto& playerArray = world->GameState->PlayerArray;
				for (SDK::int32 i = 0; i < playerArray.Num(); i++)
				{
					if (playerArray[i] == g_fakeController->PlayerState)
					{
						playerArray.Remove(i);
						LOG_DEBUG("[FakePlayer] Removed fake PlayerState from PlayerArray at index %d", i);
						break;
					}
				}
			}
		}

		// Step 1: UnPossess the pawn from the controller
		if (g_fakeController)
		{
			LOG_DEBUG("[FakePlayer] UnPossessing pawn from controller...");
			__try
			{
				g_fakeController->UnPossess();
			}
			__except (1)
			{
				LOG_WARN("[FakePlayer] Exception during UnPossess");
			}
		}

		// Step 2: Notify game mode to clean up player state via K2_OnLogout
		if (gameMode && g_fakeController)
		{
			LOG_DEBUG("[FakePlayer] Calling GameMode->K2_OnLogout on fake controller...");
			__try
			{
				gameMode->K2_OnLogout(g_fakeController);
			}
			__except (1)
			{
				LOG_WARN("[FakePlayer] Exception during K2_OnLogout");
			}
		}

		// Step 2b: Destroy the PlayerState if it exists
		if (g_fakeController && g_fakeController->PlayerState)
		{
			LOG_DEBUG("[FakePlayer] Destroying PlayerState...");
			__try
			{
				g_fakeController->PlayerState->K2_DestroyActor();
			}
			__except (1)
			{
				LOG_WARN("[FakePlayer] Exception during PlayerState K2_DestroyActor");
			}
		}

		// Step 3: Destroy the pawn actor
		if (g_fakePawn)
		{
			LOG_DEBUG("[FakePlayer] Destroying fake pawn actor...");
			__try
			{
				g_fakePawn->K2_DestroyActor();
			}
			__except (1)
			{
				LOG_WARN("[FakePlayer] Exception during pawn K2_DestroyActor");
			}
		}

		// Step 4: Remove from PlayerControllerList, then destroy controller
		if (g_fakeController)
		{
			if (g_removeControllerFn && world)
			{
				LOG_DEBUG("[FakePlayer] Calling UWorld::RemoveController...");
				__try
				{
					g_removeControllerFn(world, g_fakeController);
					LOG_DEBUG("[FakePlayer] UWorld::RemoveController completed");
				}
				__except (1)
				{
					LOG_WARN("[FakePlayer] Exception during UWorld::RemoveController");
				}
			}
			else
			{
				LOG_WARN("[FakePlayer] UWorld::RemoveController unavailable — skipping controller list cleanup");
			}

			LOG_DEBUG("[FakePlayer] Destroying fake controller actor...");
			__try
			{
				g_fakeController->K2_DestroyActor();
			}
			__except (1)
			{
				LOG_WARN("[FakePlayer] Exception during controller K2_DestroyActor");
			}
		}

		// Step 5: Clear pointers and state
		g_fakePawn = nullptr;
		g_fakeController = nullptr;
		g_playerActive = false;

		LOG_INFO("[FakePlayer] Fake player fully despawned and cleaned up");
	}

	// ---------------------------------------------------------------
	// Map traversal - teleport the fake player across waypoints
	// ---------------------------------------------------------------

	void StartMapTraversal()
	{
		LOG_INFO("[FakePlayer] StartMapTraversal() - enter");

		if (!g_playerActive || !g_fakePawn)
		{
			LOG_WARN("[FakePlayer] Cannot start traversal - fake player not active (active=%d, pawn=%p)",
			         g_playerActive, g_fakePawn);
			return;
		}

		if (g_traversing)
		{
			LOG_DEBUG("[FakePlayer] Traversal already running");
			return;
		}

		// Build the waypoint grid
		LOG_INFO("[FakePlayer] Building waypoint grid from world actors...");
		g_waypoints = BuildWaypointGridFromWorldActors();
		g_waypointIndex = 0;
		g_traversing = true;

		LOG_INFO("[FakePlayer] Server is teleporting the fake player around the map...");
		LOG_INFO("             This may cause lag until the process is complete.");

		LOG_INFO("[FakePlayer] %zu waypoints, %d per tick",
		         g_waypoints.size(), KeepTickingConfig::Config::GetWaypointsPerTick());
	}

	void StopMapTraversal()
	{
		if (!g_traversing)
			return;

		g_traversing = false;
		LOG_INFO("[FakePlayer] Map traversal STOPPED at waypoint %d/%zu",
		         g_waypointIndex, g_waypoints.size());
	}

	bool IsTraversing()
	{
		return g_traversing;
	}

	void MoveFakePlayerFarAway()
	{
		//X=-178601.10 Y=213682.05 Z=100.00
		// Teleport the pawn
		SDK::FVector newLocation;
		newLocation.X = -178601.10;
		newLocation.Y = 213682.05;
		newLocation.Z = 100.00;

		bool success = SafeSetActorLocation(g_fakePawn, newLocation);
		if (success)
		{
			LOG_INFO("[FakePlayer] Fake player moved to far away location after traversal");
		}
		else
		{
			LOG_WARN("[FakePlayer] Failed to move fake player to far away location after traversal");
		}
	}

	void TickTraversal()
	{
		if (!g_traversing || !g_fakePawn || !g_playerActive)
			return;

		if (g_waypointIndex >= static_cast<int>(g_waypoints.size()))
		{
			// Completed full pass
			LOG_INFO("[FakePlayer] Map traversal COMPLETE - visited all %zu waypoints", g_waypoints.size());
			g_traversing = false;
			return;
		}

		int waypointsPerTick = KeepTickingConfig::Config::GetWaypointsPerTick();
		int total = static_cast<int>(g_waypoints.size());

		for (int step = 0; step < waypointsPerTick && g_waypointIndex < total; step++)
		{
			const Waypoint& wp = g_waypoints[g_waypointIndex];

			// Teleport the pawn
			SDK::FVector newLocation;
			newLocation.X = wp.x;
			newLocation.Y = wp.y;
			newLocation.Z = wp.z;

			bool success = SafeSetActorLocation(g_fakePawn, newLocation);
			if (!success && g_waypointIndex == 0)
			{
				LOG_ERROR(
					"[FakePlayer] SafeSetActorLocation FAILED/EXCEPTION at waypoint %d (%.0f, %.0f, %.0f) - stopping traversal",
					g_waypointIndex, wp.x, wp.y, wp.z);
				g_traversing = false;
				return;
			}

			// Log progress periodically (every 10 waypoints or first/last)
			if (g_waypointIndex == 0 || g_waypointIndex == total - 1 || (g_waypointIndex % 50) == 0)
			{
				int pct = (g_waypointIndex * 100) / total;
				LOG_DEBUG("[FakePlayer] Traversal %d/%d (%d%%) -> (%.0f, %.0f, %.0f) %s",
				          g_waypointIndex + 1, total, pct,
				          wp.x, wp.y, wp.z,
				          success ? "OK" : "FAIL");
			}

			g_waypointIndex++;
		}

		if (g_waypointIndex >= total)
		{
			LOG_INFO("[FakePlayer] Map traversal COMPLETE - visited all %zu waypoints", g_waypoints.size());
			g_traversing = false;

			MoveFakePlayerFarAway();
		}
	}

	// Use this to prevent the spawner from activating if the fake player is already active.
	bool PreventSpawnerActivation(void* spawner)
	{
		if (!g_playerActive || !g_fakePawn)
			return false;

		auto obj = static_cast<SDK::UObject*>(spawner);

		if (obj->IsA(SDK::AMassSpawner::StaticClass()))
		{
			auto massSpawner = static_cast<SDK::AMassSpawner*>(spawner);
			SDK::FVector loc{};
			SafeGetActorLocation(massSpawner, loc);
			LOG_DEBUG("[FakePlayer] Blocking AMassSpawner '%s' at (%.0f, %.0f, %.0f) - fake player active",
			          massSpawner->GetName().c_str(), loc.X, loc.Y, loc.Z);
		}
		else if (obj->IsA(SDK::AAbstractMassEnemySpawner::StaticClass()))
		{
			auto enemySpawner = static_cast<SDK::AAbstractMassEnemySpawner*>(spawner);
			SDK::FVector loc{};
			SafeGetActorLocation(enemySpawner, loc);
			LOG_DEBUG("[FakePlayer] Blocking AAbstractMassEnemySpawner '%s' at (%.0f, %.0f, %.0f) - fake player active",
			          enemySpawner->GetName().c_str(), loc.X, loc.Y, loc.Z);
		}
		else
		{
			LOG_DEBUG("[FakePlayer] Blocking unknown spawner activation (%p) - fake player active", spawner);
		}

		return true;
	}

	// ---------------------------------------------------------------

	bool Install()
	{
		LOG_INFO("FakePlayer: Spawn/despawn system ready");

		IPluginScanner* scanner = GetScanner();
		if (scanner)
		{
			uintptr_t addr = scanner->FindPatternInMainModule(
				"48 89 54 24 ?? 48 89 4C 24 ?? 53 56 57 41 54 48 81 EC");

			if (addr)
			{
				g_removeControllerFn = reinterpret_cast<UWorld_RemoveController_t>(addr);
				LOG_INFO("[FakePlayer] UWorld::RemoveController found at 0x%llX",
				         static_cast<unsigned long long>(addr));
			}
			else
			{
				LOG_ERROR("[FakePlayer] UWorld::RemoveController pattern not found — disabling plugin");
				return false;
			}
		}

		return true;
	}

	void Remove()
	{
		// CRITICAL FIX: During shutdown, just clear pointers
		if (g_playerActive)
		{
			LOG_INFO("[FakePlayer] Shutdown: clearing fake player pointers");
			g_traversing = false;
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

	void* GetFakeController()
	{
		return g_fakeController;
	}

	void SetDebugVisibleMode(bool enabled)
	{
		g_debugVisibleMode = enabled;
		LOG_INFO("[FakePlayer] Debug visible mode %s", enabled ? "ENABLED" : "DISABLED");
	}
}
