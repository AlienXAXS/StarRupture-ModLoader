#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "cheats/player/player_cheats.h"
#include "cheats/world/world_cheats.h"
#include "ui/cheats_panel.h"

// Global plugin interface pointers
static IPluginLogger* g_logger  = nullptr;
static IPluginConfig* g_config  = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks* g_hooks    = nullptr;

IPluginLogger*  GetLogger()  { return g_logger; }
IPluginConfig*  GetConfig()  { return g_config; }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks*   GetHooks()   { return g_hooks; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1"
#endif

static PluginInfo s_pluginInfo = {
    "SRCheats",
    MODLOADER_BUILD_TAG,
    "AlienX",
    "ImGui-based cheat menu: god mode, item spawning, rupture control and more",
    PLUGIN_INTERFACE_VERSION
};

extern "C" {

    __declspec(dllexport) PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks)
    {
        g_logger  = logger;
        g_config  = config;
        g_scanner = scanner;
        g_hooks   = hooks;

        LOG_INFO("SRCheats initializing...");

        SRCheatsConfig::Config::Initialize(config);

        if (!SRCheatsConfig::Config::IsEnabled())
        {
            LOG_WARN("SRCheats is disabled in config");
            return true;
        }

        PlayerCheats::Init(hooks);
        WorldCheats::Init(hooks);

        // UI and Input are client-only; nullptr on server/generic builds
        if (hooks->UI != nullptr)
        {
            CheatsPanel::Init(hooks);
            LOG_DEBUG("Client build detected - UI panel registered");
        }
        else
        {
            LOG_DEBUG("No UI interface - running without cheat panel (server build)");
        }

        LOG_INFO("SRCheats initialized successfully");
        return true;
    }

    __declspec(dllexport) void PluginShutdown()
    {
        LOG_INFO("SRCheats shutting down...");

        if (g_hooks)
        {
            if (g_hooks->UI != nullptr)
                CheatsPanel::Shutdown(g_hooks);

            WorldCheats::Shutdown(g_hooks);
            PlayerCheats::Shutdown(g_hooks);
        }

        g_logger  = nullptr;
        g_config  = nullptr;
        g_scanner = nullptr;
        g_hooks   = nullptr;
    }

} // extern "C"
