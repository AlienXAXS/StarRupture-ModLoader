#include "mod_core.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "hooks/fake_player/fake_player.h"
#include "Engine_classes.hpp"

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

	// Spawn fake player to trick the game into staying active
	Hooks::FakePlayer::SpawnFakePlayer();
}

// ---------------------------------------------------------------------------
// Callback when engine initializes
// ---------------------------------------------------------------------------

static void OnEngineInitialized()
{
}

// ---------------------------------------------------------------------------
// Initialize — called immediately from DllMain background thread
// ---------------------------------------------------------------------------

void ModCore::Initialize(IPluginScanner* scanner, IPluginHooks* hooks)
{
	LOG_INFO("ModCore initializing...");
	
	// Register for world begin play events
	if (hooks)
	{
		hooks->RegisterWorldBeginPlayCallback(OnWorldBeginPlay);
		LOG_DEBUG("Registered for WorldBeginPlay events");
	}

	// Initialize fake player hook
	if (!Hooks::FakePlayer::Install())
	{
		LOG_WARN("Failed to install FakePlayer hook");
	}
}

void ModCore::Shutdown()
{
	// Unregister callbacks
	if (auto hooks = GetHooks())
	{
		hooks->UnregisterWorldBeginPlayCallback(OnWorldBeginPlay);
	}

	Hooks::FakePlayer::Remove();
	LOG_INFO("ModCore shutdown complete");
}
