#include "hook_management.h"
#include "globals.h"
#include "engine_sync.h"
#include "../logging/logger.h"
#include "../UI/splash_window.h"
#include "../hooks/game/actor_begin_play/actor_begin_play.h"
#include "../hooks/game/engine_init/engine_init.h"
#include "../hooks/game/engine_shutdown/engine_shutdown.h"
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

void InstallAllHooks()
{
    ModLoaderLogger::LogMessage(L"Installing core game hooks...");

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
    Hooks::TextLocalization::Remove();
    Hooks::TextKey::Remove();
#ifdef MODLOADER_SERVER_BUILD
    Hooks::HttpServer::Remove();
#endif
}
