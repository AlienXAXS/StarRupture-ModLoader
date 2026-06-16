#ifdef MODLOADER_CLIENT_BUILD

#include "client_ui.h"
#include "startup_utils.h"
#include "Engine_classes.hpp"
#include "../hooks/game/world_begin_play/world_begin_play.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../hooks/input/input_processor.h"
#include "../hooks/input/input_hook.h"
#include "../hooks/input/keybind_registry.h"
#include "../UI/global_settings.h"
#include "../UI/imgui_backend.h"
#include "../UI/imgui_host_interface.h"
#include "../UI/modloader_window.h"
#include "../UI/overlay.h"
#include "../UI/plugin_panel_registry.h"
#include "../UI/plugin_widget_registry.h"
#include "../UI/splash_window.h"
#include "../logging/log.h"

static bool         s_imguiEnabled = true;
static SDK::UWorld* s_currentWorld = nullptr;
static EModKey      s_openKey      = EModKey::F2;

void InitClientUI()
{
    const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");
    int val = GetPrivateProfileIntW(L"UI", L"Enabled", -1, iniPath.c_str());
    if (val == -1)
    {
        WritePrivateProfileStringW(L"UI", L"Enabled", L"1", iniPath.c_str());
        val = 1;
    }
    s_imguiEnabled = (val != 0);

    if (s_imguiEnabled)
    {
        UI::GlobalSettings::Load(iniPath.c_str());

        // Read open-key; default F2. Key names are ASCII so wchar cast is safe.
        wchar_t openKeyW[32] = L"F2";
        GetPrivateProfileStringW(L"UI", L"OpenKey", L"F2", openKeyW, 32, iniPath.c_str());
        char openKeyBuf[32] = {};
        for (int i = 0; i < 31 && openKeyW[i]; ++i)
            openKeyBuf[i] = static_cast<char>(openKeyW[i]);

        s_openKey = Hooks::Input::NameToModKey(openKeyBuf);
        if (s_openKey == EModKey::Unknown) s_openKey = EModKey::F2;
        UI::Overlay::SetOpenKeyName(openKeyBuf);
        Hooks::Input::RegisterKeybind(s_openKey, EModKeyEvent::Pressed,
            [](EModKey, EModKeyEvent) { UI::ModLoaderWindow::Toggle(); });

        ImGuiRenderCallbacks cbs{};
        cbs.RenderFrame = [](IModLoaderImGui* api)
        {
            UI::Overlay::Render();
            UI::Overlay::RenderHud();
            UI::ModLoaderWindow::Render(api);
            UI::PluginPanelRegistry::RenderPanelWindows(api);
            UI::PluginWidgetRegistry::RenderWidgets(api);
        };
        cbs.ShouldCaptureInput = []() -> bool
        {
            return UI::ModLoaderWindow::IsOpen();
        };
        cbs.DispatchKey = [](UINT msg, WPARAM wParam, LPARAM lParam) -> bool
        {
            return Hooks::Input::ProcessWindowMessage(msg, wParam, lParam);
        };
        cbs.GetFontFamily = []() -> const char*
        {
            return UI::GlobalSettings::GetFontFamily();
        };
        cbs.GetFontScale = []() -> float
        {
            return UI::GlobalSettings::GetFontScale();
        };
        cbs.Log = [](int level, const char* msg)
        {
            switch (level)
            {
            case 0: LogToFile::Trace("%s", msg);  break;
            case 1: LogToFile::Debug("%s", msg);  break;
            case 2: LogToFile::Info("%s", msg);   break;
            case 3: LogToFile::Warn("%s", msg);   break;
            default: LogToFile::Error("%s", msg); break;
            }
        };
        cbs.OnDeviceLost = [](const wchar_t* /*msg*/)
        {
            Splash::Show();
        };
        cbs.RemoveInputHook = []()
        {
            Hooks::InputHook::Remove();
        };

        UI::ImGuiBackend::Initialize(cbs);
    }

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
