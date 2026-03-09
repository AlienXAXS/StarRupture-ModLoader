#include "mod_core.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "hooks/fake_player/fake_player.h"
#include "sdk_helpers.h"
#include "Engine_classes.hpp"

// ---------------------------------------------------------------------------
// Helper: query the engine for the real (networked) player count.
// Uses NetDriver->ClientConnections which only counts actual network
// connections — the locally-spawned fake player is never included.
// ---------------------------------------------------------------------------
static int GetRealPlayerCount()
{
	int count = SDKHelpers::GetPlayerCount();
	if (count < 0)
	{
		LOG_WARN("[ModCore] GetPlayerCount returned %d — NetDriver may not be ready yet", count);
		return 0;
	}
	return count;
}

// ---------------------------------------------------------------------------
// Callback when world begins play (ChimeraMain world)
// ---------------------------------------------------------------------------

static void OnWorldBeginPlay(SDK::UWorld* world)
{
	LOG_INFO("ChimeraMain world begin play - spawning fake player...");

	if (!world)
	{
		LOG_ERROR("World is null in callback!");
		return;
	}

	// Set debug visible mode from config before spawning
	bool debugVisible = KeepTickingConfig::Config::IsDebugVisibleModeEnabled();
	Hooks::FakePlayer::SetDebugVisibleMode(debugVisible);

	bool shouldPreventSleep = KeepTickingConfig::Config::ShouldPreventServerSleep();
	// Spawn fake player to trick the game into staying active
	if (shouldPreventSleep)
		Hooks::FakePlayer::SpawnFakePlayer();
}

// ---------------------------------------------------------------------------
// Callback when experience finishes loading (all gameplay ready)
// ---------------------------------------------------------------------------

static void OnExperienceLoadComplete()
{
	LOG_INFO("[ModCore] ExperienceLoadComplete fired — starting map traversal");

	if (!Hooks::FakePlayer::IsPlayerActive())
	{
		LOG_WARN("[ModCore] Fake player not active, cannot start traversal");
		return;
	}

	Hooks::FakePlayer::StartMapTraversal();
}

// ---------------------------------------------------------------------------
// Callback every engine tick — drives the traversal as fast as possible
// ---------------------------------------------------------------------------

static void OnEngineTick(float deltaSeconds)
{
	Hooks::FakePlayer::TickTraversal();
}

// ---------------------------------------------------------------------------
// Callback when engine initializes
// ---------------------------------------------------------------------------

static void OnEngineInitialized()
{
}

// ---------------------------------------------------------------------------
// Callback when a player joins (ACrGameModeBase::PostLogin)
// ---------------------------------------------------------------------------

static void OnPlayerJoined(void* playerController)
{
	if (!KeepTickingConfig::Config::ShouldPreventServerSleep())
		return;

	// Ignore the fake player's own PostLogin
	if (playerController == Hooks::FakePlayer::GetFakeController())
	{
		LOG_DEBUG("[ModCore] PlayerJoined: ignoring fake player controller %p", playerController);
		return;
	}

	int realPlayers = GetRealPlayerCount();
	LOG_INFO("[ModCore] PlayerJoined: real player count from engine is %d", realPlayers);

	// A real player is now connected — remove the fake player
	// (realPlayers >= 1 because PostLogin fires after the connection is established)
	if (realPlayers >= 1 && Hooks::FakePlayer::IsPlayerActive())
	{
		LOG_INFO("[ModCore] Real player present — despawning fake player");
		Hooks::FakePlayer::StopMapTraversal();
		Hooks::FakePlayer::DespawnFakePlayer();
	}
}

// ---------------------------------------------------------------------------
// Callback when a player leaves (ACrGameModeBase::Logout)
// ---------------------------------------------------------------------------

static void OnPlayerLeft(void* exitingController)
{
	if (!KeepTickingConfig::Config::ShouldPreventServerSleep())
		return;

	// Ignore the fake player's own Logout
	if (exitingController == Hooks::FakePlayer::GetFakeController())
	{
		LOG_DEBUG("[ModCore] PlayerLeft: ignoring fake player controller %p", exitingController);
		return;
	}

	// Logout callbacks fire BEFORE the original — so the departing player
	// is still counted in ClientConnections at this point.  A count of 1
	// means the departing player is the last one.
	int realPlayers = GetRealPlayerCount();
	LOG_INFO("[ModCore] PlayerLeft: real player count from engine is %d (departing player still counted)", realPlayers);

	// Last real player is leaving — respawn the fake player
	if (realPlayers <= 1 && !Hooks::FakePlayer::IsPlayerActive())
	{
		LOG_INFO("[ModCore] Last real player leaving — respawning fake player");

		bool debugVisible = KeepTickingConfig::Config::IsDebugVisibleModeEnabled();
		Hooks::FakePlayer::SetDebugVisibleMode(debugVisible);

		Hooks::FakePlayer::SpawnFakePlayer();

		// Restart map traversal if the player is now active
		if (Hooks::FakePlayer::IsPlayerActive())
		{
			LOG_INFO("[ModCore] Fake player respawned — restarting map traversal");
			Hooks::FakePlayer::StartMapTraversal();
		}
	}
}

// ---------------------------------------------------------------------------
// Initialize — called immediately from DllMain background thread
// ---------------------------------------------------------------------------

void ModCore::Initialize(IPluginScanner* scanner, IPluginHooks* hooks)
{
	LOG_INFO("ModCore initializing...");

	// Initialize fake player hook first — required patterns must be found before we register any callbacks
	if (!Hooks::FakePlayer::Install())
	{
		LOG_ERROR("FakePlayer install failed — required patterns not found, plugin disabled");
		return;
	}

	// Register for world begin play events
	if (hooks)
	{
		if (hooks->World)
		{
			hooks->World->RegisterOnWorldBeginPlay(OnWorldBeginPlay);
			LOG_DEBUG("Registered for WorldBeginPlay events");

			hooks->World->RegisterOnExperienceLoadComplete(OnExperienceLoadComplete);
			LOG_DEBUG("Registered for ExperienceLoadComplete callback (map traversal)");
		}

		if (hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnEngineTick);
			LOG_DEBUG("Registered for EngineTick callback (traversal driver)");
		}

		if (hooks->Players)
		{
			hooks->Players->RegisterOnPlayerJoined(OnPlayerJoined);
			LOG_DEBUG("Registered for PlayerJoined callback (fake player management)");

			hooks->Players->RegisterOnPlayerLeft(OnPlayerLeft);
			LOG_DEBUG("Registered for PlayerLeft callback (fake player management)");
		}
	}
}

void ModCore::Shutdown()
{
	// Stop traversal before unregistering
	Hooks::FakePlayer::StopMapTraversal();

	// Unregister callbacks
	if (auto hooks = GetHooks())
	{
		if (hooks->World)
		{
			hooks->World->UnregisterOnWorldBeginPlay(OnWorldBeginPlay);
			hooks->World->UnregisterOnExperienceLoadComplete(OnExperienceLoadComplete);
		}

		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);

		if (hooks->Players)
		{
			hooks->Players->UnregisterOnPlayerJoined(OnPlayerJoined);
			hooks->Players->UnregisterOnPlayerLeft(OnPlayerLeft);
		}
	}

	Hooks::FakePlayer::Remove();
	LOG_INFO("ModCore shutdown complete");
}
