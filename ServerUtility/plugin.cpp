#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "hooks/parse_settings/parse_settings.h"
#include "hooks/max_players/max_players.h"
#include "hooks/auto_profession/auto_profession.h"
#include "hooks/http_connection/http_connection.h"
#include "rcon/rcon.h"
#include "rcon/console_ctrl.h"
#include "rcon/commands/command_handler.h"

// -----------------------------------------------------------------------
// Global plugin self pointer
// -----------------------------------------------------------------------
static IPluginSelf* g_self = nullptr;

// Getter used by plugin_helpers.h macros and hook implementations
IPluginSelf* GetSelf() { return g_self; }

// -----------------------------------------------------------------------
// Plugin metadata
// -----------------------------------------------------------------------
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.4"
#endif

static PluginInfo s_pluginInfo = {
	"ServerUtility",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Provides dedicated-server settings via command-line parameters, bypassing DSSettings.txt as well as other various fixes",
	PLUGIN_INTERFACE_VERSION
};

// -----------------------------------------------------------------------
// UCrDedicatedServerSettingsComp::ParseSettings pattern
// "48 8B C4 55 41 54 48 8D 6C 24"
// -----------------------------------------------------------------------
static constexpr auto DEDSERVER_SETTINGS_COMP_PARSE_SETTINGS_PATTERN = "48 8B C4 55 41 54 48 8D 6C 24";

// -----------------------------------------------------------------------
// Engine lifecycle callbacks
// -----------------------------------------------------------------------
static void OnEngineInit()
{
	// Start the RCON / Steam Query subsystem
	Rcon::Init();

	// Install console control handler (CTRL+C, CTRL+BREAK, window close, etc.)
	ConsoleCtrl::Install();

	// Install FHttpConnection::ProcessRequest hook
	HttpConnectionHook::Install();
}

static void OnEngineShutdown()
{
	LOG_INFO("Engine shutting down - removing hooks...");
	ConsoleCtrl::Remove();
	ParseSettingsHook::Remove();
	MaxPlayersHook::Remove();
	AutoProfessionHook::Remove();
	HttpConnectionHook::Remove();

	// Shut down RCON / Steam Query (must happen before UObject teardown)
	Rcon::Shutdown();
}

static bool IsServerBinary()
{
	return g_self->hooks->Network->IsServer();
}

// -----------------------------------------------------------------------
// Plugin exports
// -----------------------------------------------------------------------
extern "C" {
__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;

	LOG_INFO("Plugin initialising...");

	// Initialize config system with schema
	ServerUtilityConfig::Config::Initialize(self);

	if (!ServerUtilityConfig::Config::IsPluginEnabled())
	{
		LOG_INFO("Plugin is disabled in config - skipping initialization");
		return true;
	}

	if (!IsServerBinary())
	{
		LOG_WARN("This plugin is intended for dedicated server use only - skipping initialization");
		return true;
	}

	if (!g_self->hooks->Engine)
	{
		LOG_ERROR("Engine sub-interface not available - loader version mismatch?");
		return false;
	}

	// Give the command handler access to the hooks so it can dispatch game-thread
	// commands via hooks->Engine->PostToGameThread.
	CommandHandler::Get().SetHooks(g_self->hooks);

	g_self->hooks->Engine->RegisterOnInit(OnEngineInit);
	LOG_DEBUG("Registered for engine init callback");

	g_self->hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
	LOG_DEBUG("Registered for engine shutdown callback");

	LOG_INFO("Engine initialised - scanning for UCrDedicatedServerSettingsComp::ParseSettings...");

	uintptr_t addr = g_self->scanner->FindPatternInMainModule(DEDSERVER_SETTINGS_COMP_PARSE_SETTINGS_PATTERN);
	if (addr == 0)
	{
		LOG_ERROR("Pattern scan failed – could not locate ParseSettings");
	}
	else
	{
		LOG_INFO("Found ParseSettings at 0x%llX", static_cast<unsigned long long>(addr));
		ParseSettingsHook::Install(addr);
	}

	// Apply max players patch if configured
	int maxPlayers = ServerUtilityConfig::Config::GetMaxPlayers();
	if (maxPlayers > 0)
	{
		LOG_INFO("MaxPlayers configured to %d - applying patch...", maxPlayers);
		MaxPlayersHook::Install(maxPlayers);

		// Auto-assign professions for players joining when MaxPlayers is patched
		AutoProfessionHook::Install();
	}

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");

	// Hook removal and RCON shutdown are handled in OnEngineShutdown() which fires
	// before UObject teardown.  By the time PluginShutdown is called (explicit
	// FreeLibrary only) those resources have already been released.

	g_self = nullptr;
}
} // extern "C"
