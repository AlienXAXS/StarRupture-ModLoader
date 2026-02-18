#include "mod_core.h"
#include "plugin_logger.h"
#include "plugin_scanner.h"
#include "plugin_hooks.h"
#include "hooks/engine_init/engine_init.h"
#include "hooks/fake_player/fake_player.h"
#include "SDK/Engine_classes.hpp"
// #include "hooks/fragment_reparent/fragment_reparent.h"  // TODO: File missing

// ---------------------------------------------------------------------------
// Callback when world begins play (ChimeraMain world)
// ---------------------------------------------------------------------------

static void OnWorldBeginPlay(SDK::UWorld* world)
{
	PluginLogger::Info("ChimeraMain world begin play - spawning fake player...");

	if (!world)
	{
		PluginLogger::Error("World is null in callback!");
		return;
	}

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
	PluginLogger::Info("ModCore initializing...");
	
	// Initialize the scanner wrapper
	PluginScanner::Initialize(scanner);
	
	// Initialize the hooks wrapper
	PluginHooks::Initialize(hooks);
}

void ModCore::Shutdown()
{
	Hooks::FakePlayer::Remove();
	PluginLogger::Info("ModCore shutdown complete");
}
