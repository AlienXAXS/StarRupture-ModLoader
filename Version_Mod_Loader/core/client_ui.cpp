#ifdef MODLOADER_CLIENT_BUILD

#include "client_ui.h"
#include "startup_utils.h"
#include "Engine_classes.hpp"
#include "../hooks/game/world_begin_play/world_begin_play.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../hooks/input/input_processor.h"
#include "../UI/global_settings.h"
#include "../UI/imgui_backend.h"
#include "../UI/overlay.h"

static bool         s_imguiEnabled = true;
static SDK::UWorld* s_currentWorld = nullptr;

void InitClientUI()
{
    const std::wstring iniPath = GetExeDirPath(L"modloader.ini");
    int val = GetPrivateProfileIntW(L"UI", L"Enabled", -1, iniPath.c_str());
    if (val == -1)
    {
        WritePrivateProfileStringW(L"UI", L"Enabled", L"1", iniPath.c_str());
        val = 1;
    }
    s_imguiEnabled = (val != 0);

    if (s_imguiEnabled)
        UI::ImGuiBackend::Initialize();

    static auto s_onWorldReady = [](SDK::UWorld* world, const char* worldName)
    {
        s_currentWorld = world;

        const bool isMainMenu = worldName && strstr(worldName, "Map_MainMenu") != nullptr;
        UI::Overlay::SetVisible(isMainMenu);
        UI::GlobalSettings::SetWorldName(worldName ? worldName : "");
    };
    Hooks::WorldBeginPlay::RegisterAnyWorldCallback(s_onWorldReady);

    static auto s_onTick = [](float /*deltaSeconds*/)
    {
        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(s_currentWorld, 0);
        if (!pc)
        {
            UI::GlobalSettings::SetPlayerPosition(0, 0, 0, false);
            return;
        }
        SDK::APawn* pawn = pc->K2_GetPawn();
        if (!pawn)
        {
            UI::GlobalSettings::SetPlayerPosition(0, 0, 0, false);
            return;
        }
        SDK::FVector loc = pawn->K2_GetActorLocation();
        UI::GlobalSettings::SetPlayerPosition(loc.X, loc.Y, loc.Z, true);
    };
    Hooks::EngineTick::RegisterPluginCallback(s_onTick);
}

void ShutdownClientUI()
{
    if (s_imguiEnabled)
        UI::ImGuiBackend::Shutdown();
    Hooks::Input::RemoveInputProcessor();
}

#endif // MODLOADER_CLIENT_BUILD
