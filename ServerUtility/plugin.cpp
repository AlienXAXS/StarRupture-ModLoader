#include "plugin.h"
#include "plugin_helpers.h"
#include "hooks/parse_settings/parse_settings.h"

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
static PluginInfo s_pluginInfo = {
    "ServerUtility",
    "1.0.0",
    "AlienX",
    "Provides dedicated-server settings via command-line parameters, bypassing DSSettings.txt",
    PLUGIN_INTERFACE_VERSION
};

// -----------------------------------------------------------------------
// UCrDedicatedServerSettingsComp::ParseSettings pattern
// "48 8B C4 55 41 54 48 8D 6C 24"
// -----------------------------------------------------------------------
static constexpr const char* PARSE_SETTINGS_PATTERN = "48 8B C4 55 41 54 48 8D 6C 24";

// -----------------------------------------------------------------------
// Engine lifecycle callbacks
// -----------------------------------------------------------------------
static void OnEngineInit()
{
    LOG_INFO("Engine initialised – scanning for UCrDedicatedServerSettingsComp::ParseSettings...");

    uintptr_t addr = g_scanner->FindPatternInMainModule(PARSE_SETTINGS_PATTERN);
    if (addr == 0)
    {
        LOG_ERROR("Pattern scan failed – could not locate ParseSettings");
        return;
    }

    LOG_INFO("Found ParseSettings at 0x%llX", static_cast<unsigned long long>(addr));
    ParseSettingsHook::Install(addr);
}

static void OnEngineShutdown()
{
    LOG_INFO("Engine shutting down – removing ParseSettings hook...");
    ParseSettingsHook::Remove();
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

        if (!hooks->RegisterEngineInitCallback)
        {
            LOG_ERROR("RegisterEngineInitCallback not available – loader version mismatch?");
            return false;
        }

        hooks->RegisterEngineInitCallback(OnEngineInit);
        LOG_INFO("Registered for engine init callback");

        if (hooks->RegisterEngineShutdownCallback)
        {
            hooks->RegisterEngineShutdownCallback(OnEngineShutdown);
            LOG_INFO("Registered for engine shutdown callback");
        }
        else
        {
            LOG_WARN("RegisterEngineShutdownCallback not available – hook may not be removed cleanly on shutdown");
        }

        LOG_INFO("Plugin initialised. Awaiting engine ready signal.");
        LOG_INFO("Usage: launch the server with the following parameters:");
        LOG_INFO("  -SessionName=<name> [-SaveGameInterval=<seconds>]");
        LOG_INFO("When SessionName is present, DSSettings.txt is completely bypassed.");
        LOG_INFO("  SaveGameName: Always 'AutoSave0.sav' (fixed)");
        LOG_INFO("  SaveGameInterval: Defaults to 300 seconds (5 minutes) if not specified");
        LOG_INFO("  StartNewGame / LoadSavedGame: Derived automatically from save file existence");
        LOG_INFO("Save location checked: <binDir>\\..\\..\\Saved\\SaveGames\\<SessionName>\\AutoSave0.sav");
        LOG_INFO("  (navigates up 2 directories from binary: Win64 -> Binaries -> <root>)");

        return true;
    }

    __declspec(dllexport) void PluginShutdown()
    {
        LOG_INFO("Plugin shutting down...");

        // Hook removal is handled in OnEngineShutdown() which fires before UObject
        // teardown.  By the time PluginShutdown is called (explicit FreeLibrary only)
        // the hook has already been removed – do not touch engine memory here.

        g_logger  = nullptr;
        g_config  = nullptr;
        g_scanner = nullptr;
        g_hooks   = nullptr;
    }

} // extern "C"
