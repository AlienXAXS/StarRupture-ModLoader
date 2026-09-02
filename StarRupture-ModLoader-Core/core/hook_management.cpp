#include "hook_management.h"
#include "globals.h"
#include "engine_sync.h"
#include "../logging/logger.h"
#include "../UI/splash_window.h"
#include "../hooks/game/actor_begin_play/actor_begin_play.h"
#include "../hooks/game/crafting_finished/crafting_finished.h"
#include "../hooks/game/engine_init/engine_init.h"
#include "../hooks/game/engine_shutdown/engine_shutdown.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../hooks/game/experience_load_complete/experience_load_complete.h"
#include "../hooks/game/game_instance_init/game_instance_init.h"
#include "../hooks/game/mass_do_spawning/mass_do_spawning.h"
#include "../hooks/game/object_lookup/object_lookup.h"
#include "../hooks/game/mass_spawner_activate/mass_spawner_activate.h"
#include "../hooks/game/mass_spawner_deactivate/mass_spawner_deactivate.h"
#include "../hooks/game/player_joined/player_joined.h"
#include "../hooks/game/player_left/player_left.h"
#include "../hooks/game/save_loaded/save_loaded.h"
#include "../hooks/game/text_localization/text_localization.h"
#include "../hooks/game/text_localization/text_key.h"
#include "../hooks/game/world_begin_play/world_begin_play.h"
#include "../hooks/game/world_end_play/world_end_play.h"
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#include "../hooks/game/session_info/session_info.h"
#endif

#ifdef MODLOADER_SERVER_BUILD
#include "../hooks/http/http_server_hook.h"
#endif

#ifdef MODLOADER_CLIENT_BUILD
#include "../hooks/game/crash_reporter/crash_reporter.h"
#include "../hooks/game/hud_post_render/hud_post_render.h"
#include "../hooks/game/log_verbosity/log_verbosity.h"
#endif

void InstallAllHooks()
{
    ModLoaderLogger::LogMessage(L"Installing core game hooks...");

#ifdef MODLOADER_CLIENT_BUILD
    // First: this detours FLogSuppressionImplementation::ProcessConfigAndCommandLine,
    // which runs during FEngineLoop::PreInit. Installing it before anything else gives
    // the best chance of landing ahead of PreInit and having the override survive.
    Splash::SetStatus(L"Installing LogVerbosity hook...");

    if (Hooks::LogVerbosity::Install())
        ModLoaderLogger::LogDebug(L"  LogVerbosity hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: LogVerbosity hook failed -- game log verbosity control unavailable");

    Splash::SetStatus(L"Installing CrashReporter hook...");

    if (Hooks::CrashReporter::Install())
        ModLoaderLogger::LogDebug(L"  CrashReporter hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: CrashReporter hook failed -- crash reports will still be sent to the developers");
#endif

    Splash::SetStatus(L"Installing EngineInit hook...");
    Splash::SetProgress(0.20f);

    Hooks::EngineInit::SetSyncEvents(g_engineReadyEvent, g_pluginsLoadedEvent);
    Hooks::EngineInit::SetUE4SSReadyEvent(g_ue4ssReadyEvent);
    Hooks::EngineInit::SetPluginsReadyEvent(g_pluginsReadyEvent);

    if (Hooks::EngineInit::Install())
    {
        ModLoaderLogger::LogDebug(L"  EngineInit hook installed");
        Hooks::EngineInit::RegisterPluginCallback(OnEngineInitForUELog);
        Hooks::EngineInit::RegisterPluginCallback(OnEngineInitForGObjectExport);
    }
    else
    {
        ModLoaderLogger::LogWarn(L"  WARNING: EngineInit hook failed to install -- loading plugins immediately");
        if (g_engineReadyEvent)
            SetEvent(g_engineReadyEvent);
        if (g_pluginsReadyEvent)
            SetEvent(g_pluginsReadyEvent);
    }

    Splash::SetStatus(L"Installing EngineShutdown hook...");
    Splash::SetProgress(0.25f);

    if (Hooks::EngineShutdown::Install())
        ModLoaderLogger::LogDebug(L"  EngineShutdown hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: EngineShutdown hook failed to install - plugins will not receive shutdown callbacks");

    Splash::SetStatus(L"Installing spawner hooks...");
    Splash::SetProgress(0.30f);

    ModLoaderLogger::LogInfo(L"[EngineInit] Installing spawner hooks...");
    Hooks::MassSpawnerActivate::Install();
    Hooks::MassSpawnerDeactivate::Install();
    Hooks::MassDoSpawning::Install();
	Hooks::ActorBeginPlay::Install();

    Splash::SetStatus(L"Installing TextLocalization hook...");

    if (Hooks::TextLocalization::Install())
        ModLoaderLogger::LogDebug(L"  TextLocalization hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: TextLocalization hook failed to install");

    if (Hooks::TextKey::Install())
        ModLoaderLogger::LogDebug(L"  TextKey hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: TextKey hook failed to install");

    Splash::SetStatus(L"Resolving object lookup functions...");

    if (Hooks::ObjectLookup::Install())
        ModLoaderLogger::LogDebug(L"  ObjectLookup functions resolved");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: ObjectLookup failed to resolve one or more object/package lookup functions");

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
    if (Hooks::SessionInfo::Install())
        ModLoaderLogger::LogDebug(L"  SessionInfo (NetMode) resolved");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: SessionInfo failed to resolve AActor::InternalGetNetMode -- IPluginNetModeInfo will report Unknown");
#endif

#ifdef MODLOADER_SERVER_BUILD
    Splash::SetStatus(L"Installing HTTP server hook...");
    if (Hooks::HttpServer::Install())
        ModLoaderLogger::LogDebug(L"  HttpServer hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: HttpServer hook failed -- static file routes and request filters will not function");
#endif

    Splash::SetStatus(L"Installing GameInstance hook...");
    Splash::SetProgress(0.40f);

    if (Hooks::GameInstanceInit::Install())
        ModLoaderLogger::LogDebug(L"  GameInstanceInit hook installed");
    else
        ModLoaderLogger::LogWarn(L"  WARNING: GameInstanceInit hook failed -- plugins will not be initialized");
}

// ---------------------------------------------------------------------------
// Plugin-facing event hooks
//
// Every hooks/game/<event>/ module installs itself lazily, on the first
// RegisterPluginCallback. That is fine for a plugin that subscribes from a game
// callback, and wrong for one that subscribes from PluginInit:
//
//  * Installing a detour patches live code. By PluginInit the engine is up and
//    the game's own threads are already running through these functions, so the
//    first plugin to register is the one that pays for the pattern scan and the
//    code patch -- and the one that eats the failure if either goes wrong.
//  * The failure is silent from the plugin's side. RegisterPluginCallback logs
//    the error and returns without adding the callback, so the plugin believes
//    it is subscribed and simply never fires. That reads as "the modloader
//    ignored my event", which is how it gets reported.
//  * Which plugin pays depends on load order, so the same hook is installed
//    from a different DLL's PluginInit depending on what else is in Plugins\.
//
// Installing the whole set here instead -- after the plugin DLLs are loaded,
// before any PluginInit runs, with the main thread still held by the engine-init
// hook -- makes registration a pure append onto a hook that is already live, and
// puts any install failure in the log under the loader's name, once, before the
// plugin that cares about it has run a line.
//
// This does NOT replace the lazy path, which is still needed for anything that
// registers later: a plugin hot-loaded from the console, and a hook whose
// pattern scan failed here but might succeed once more of the game is up. Both
// Install() and RegisterPluginCallback re-test the same installed flag, so
// running both is safe.
//
// EngineInit / EngineShutdown are deliberately absent: they are Stage 1 hooks
// that must be in place long before this point, they have no IsInstalled(), and
// Hook::Install refuses (and warns) on a second call.
// ---------------------------------------------------------------------------
namespace
{
    void InstallEventHook(const wchar_t* name, bool (*isInstalled)(), bool (*install)())
    {
        if (isInstalled())
        {
            ModLoaderLogger::LogDebug(L"  %s hook already installed", name);
            return;
        }

        if (install())
            ModLoaderLogger::LogDebug(L"  %s hook installed", name);
        else
            ModLoaderLogger::LogWarn(L"  WARNING: %s hook failed to install -- plugins subscribing "
                                     L"to this event will not be called", name);
    }
}

void InstallPluginEventHooks()
{
    Splash::SetStatus(L"Installing plugin event hooks...");
    ModLoaderLogger::LogMessage(L"Installing plugin event hooks...");

    InstallEventHook(L"WorldBeginPlay",
        &Hooks::WorldBeginPlay::IsInstalled,        &Hooks::WorldBeginPlay::Install);
    InstallEventHook(L"WorldEndPlay",
        &Hooks::WorldEndPlay::IsInstalled,          &Hooks::WorldEndPlay::Install);
    InstallEventHook(L"SaveLoaded",
        &Hooks::SaveLoaded::IsInstalled,            &Hooks::SaveLoaded::Install);
    InstallEventHook(L"ExperienceLoadComplete",
        &Hooks::ExperienceLoadComplete::IsInstalled, &Hooks::ExperienceLoadComplete::Install);
    InstallEventHook(L"EngineTick",
        &Hooks::EngineTick::IsInstalled,            &Hooks::EngineTick::Install);
    InstallEventHook(L"ActorBeginPlay",
        &Hooks::ActorBeginPlay::IsInstalled,        &Hooks::ActorBeginPlay::Install);
    InstallEventHook(L"CraftingFinished",
        &Hooks::CraftingFinished::IsInstalled,      &Hooks::CraftingFinished::Install);
    InstallEventHook(L"PlayerJoined",
        &Hooks::PlayerJoined::IsInstalled,          &Hooks::PlayerJoined::Install);
    InstallEventHook(L"PlayerLeft",
        &Hooks::PlayerLeft::IsInstalled,            &Hooks::PlayerLeft::Install);
    InstallEventHook(L"MassSpawnerActivate",
        &Hooks::MassSpawnerActivate::IsInstalled,   &Hooks::MassSpawnerActivate::Install);
    InstallEventHook(L"MassSpawnerDeactivate",
        &Hooks::MassSpawnerDeactivate::IsInstalled, &Hooks::MassSpawnerDeactivate::Install);
    InstallEventHook(L"MassDoSpawning",
        &Hooks::MassDoSpawning::IsInstalled,        &Hooks::MassDoSpawning::Install);

#ifdef MODLOADER_CLIENT_BUILD
    InstallEventHook(L"HUDPostRender",
        &Hooks::HUDPostRender::IsInstalled,         &Hooks::HUDPostRender::Install);
#endif
}

void RemoveAllHooks()
{
    ModLoaderLogger::LogInfo(L"Removing remaining core game hooks...");
    Hooks::EngineInit::Remove();
    Hooks::WorldBeginPlay::Remove();
    Hooks::WorldEndPlay::Remove();
    Hooks::SaveLoaded::Remove();
    Hooks::ExperienceLoadComplete::Remove();
    Hooks::ActorBeginPlay::Remove();
    Hooks::PlayerJoined::Remove();
    Hooks::PlayerLeft::Remove();
    Hooks::MassSpawnerActivate::Remove();
    Hooks::MassSpawnerDeactivate::Remove();
    Hooks::MassDoSpawning::Remove();
    Hooks::CraftingFinished::Remove();
    Hooks::EngineTick::Remove();
    Hooks::GameInstanceInit::Remove();
    Hooks::TextLocalization::Remove();
    Hooks::TextKey::Remove();
#ifdef MODLOADER_SERVER_BUILD
    Hooks::HttpServer::Remove();
#endif
#ifdef MODLOADER_CLIENT_BUILD
    Hooks::CrashReporter::Remove();
    Hooks::HUDPostRender::Remove();
#endif
}
