#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "hooks/parse_settings/parse_settings.h"
#include "hooks/max_players/max_players.h"
#include "hooks/auto_profession/auto_profession.h"
#include "hooks/http_connection/http_connection.h"
#include "rcon/rcon.h"

// -----------------------------------------------------------------------
// Global plugin interface pointers
// -----------------------------------------------------------------------
static IPluginLogger*  g_logger  = nullptr;
static IPluginConfig*  g_config  = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks*   g_hooks   = nullptr;

// Getters used by plugin_helpers.h macros and hook implementations
IPluginLogger*  GetLogger()  { return g_logger;  }
IPluginConfig*  GetConfig()  { return g_config;  }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks*   GetHooks()   { return g_hooks;   }

// -----------------------------------------------------------------------
// Plugin metadata
// -----------------------------------------------------------------------
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
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
static constexpr const char* DEDSERVER_SETTINGS_COMP_PARSE_SETTINGS_PATTERN = "48 8B C4 55 41 54 48 8D 6C 24";

// -----------------------------------------------------------------------
// Engine lifecycle callbacks
// -----------------------------------------------------------------------
static void OnEngineInit()
{
    // Start the RCON / Steam Query subsystem
    Rcon::Init();

    // Install FHttpConnection::ProcessRequest hook
    HttpConnectionHook::Install();
}

static void OnEngineShutdown()
{
    LOG_INFO("Engine shutting down – removing hooks...");
    ParseSettingsHook::Remove();
    MaxPlayersHook::Remove();
    AutoProfessionHook::Remove();
    HttpConnectionHook::Remove();

    // Shut down RCON / Steam Query (must happen before UObject teardown)
    Rcon::Shutdown();
}

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
    Rcon::OnAnyWorldBeginPlay(world, worldName);
}

static void OnExperienceLoadComplete()
{
    Rcon::OnExperienceLoadComplete();
}

static void OnEngineTick(float deltaSeconds)
{
    Rcon::OnTick(deltaSeconds);
}

static bool IsServerBinary()
{
    wchar_t path[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
    {
        // If desired, log failure via GetLogger(); keep simple here.
        return false;
    }

    // Extract filename part
    wchar_t* filename = wcsrchr(path, L'\\');
    if (!filename)
        filename = wcsrchr(path, L'/');

    const wchar_t* exeName = filename ? (filename + 1) : path;

    // Case-insensitive compare
    return _wcsicmp(exeName, L"StarRuptureServerEOS-Win64-Shipping.exe") == 0;
}

// -----------------------------------------------------------------------
// Plugin exports
// -----------------------------------------------------------------------
extern "C"
{
    __declspec(dllexport) PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config,
                                          IPluginScanner* scanner, IPluginHooks* hooks)
    {
        g_logger  = logger;
        g_config  = config;
        g_scanner = scanner;
        g_hooks   = hooks;

        LOG_INFO("Plugin initialising...");

        // Initialize config system with schema
        ServerUtilityConfig::Config::Initialize(config);

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

        if (!hooks->RegisterEngineInitCallback)
        {
            LOG_ERROR("RegisterEngineInitCallback not available – loader version mismatch?");
            return false;
        }

        hooks->RegisterEngineInitCallback(OnEngineInit);
        LOG_DEBUG("Registered for engine init callback");

        if (hooks->RegisterEngineShutdownCallback)
        {
            hooks->RegisterEngineShutdownCallback(OnEngineShutdown);
            LOG_DEBUG("Registered for engine shutdown callback");
        }
        else
        {
            LOG_WARN("RegisterEngineShutdownCallback not available – hook may not be removed cleanly on shutdown");
        }

        if (hooks->RegisterAnyWorldBeginPlayCallback)
        {
            hooks->RegisterAnyWorldBeginPlayCallback(OnAnyWorldBeginPlay);
            LOG_DEBUG("Registered for any-world begin play callback (RCON player tracking)");
        }

        if (hooks->RegisterExperienceLoadCompleteCallback)
        {
            hooks->RegisterExperienceLoadCompleteCallback(OnExperienceLoadComplete);
            LOG_DEBUG("Registered for experience load complete callback (RCON player refresh)");
        }

        if (hooks->RegisterEngineTickCallback)
        {
            hooks->RegisterEngineTickCallback(OnEngineTick);
            LOG_DEBUG("Registered for engine tick callback (game-thread task dispatch)");
        }

        LOG_INFO("Engine initialised - scanning for UCrDedicatedServerSettingsComp::ParseSettings...");

        uintptr_t addr = g_scanner->FindPatternInMainModule(DEDSERVER_SETTINGS_COMP_PARSE_SETTINGS_PATTERN);
        if (addr == 0)
        {
            LOG_ERROR("Pattern scan failed – could not locate ParseSettings");
        } else {
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

        if (g_hooks)
        {
            if (g_hooks->UnregisterAnyWorldBeginPlayCallback)
                g_hooks->UnregisterAnyWorldBeginPlayCallback(OnAnyWorldBeginPlay);

            if (g_hooks->UnregisterExperienceLoadCompleteCallback)
                g_hooks->UnregisterExperienceLoadCompleteCallback(OnExperienceLoadComplete);

            if (g_hooks->UnregisterEngineTickCallback)
                g_hooks->UnregisterEngineTickCallback(OnEngineTick);
        }

        g_logger  = nullptr;
        g_config  = nullptr;
        g_scanner = nullptr;
        g_hooks   = nullptr;
    }

} // extern "C"
