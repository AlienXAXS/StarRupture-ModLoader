#ifdef MODLOADER_CLIENT_BUILD

#include "client_ui.h"
#include "ui_state.h"
#include "input_arbitration.h"
#include "core/sdk_globals.h"
#include "startup_utils.h"
#include "Engine_classes.hpp"
#include "../hooks/game/text_input_focus/text_input_focus.h"
#include "../hooks/game/world_begin_play/world_begin_play.h"
#include "../hooks/game/engine_tick/engine_tick.h"
#include "../hooks/game/slate_present/slate_present.h"
#include "../hooks/game/game_menu/game_menu.h"
#include "../hooks/game/game_menu/game_menu_registry.h"
#include "../hooks/input/input_processor.h"
#include "../hooks/input/input_hook.h"
#include "../hooks/input/keybind_registry.h"
#include "../hooks/input/mouse_wheel_registry.h"
#include "../plugins/reload_policy.h"
#include "../UI/global_settings.h"
#include "../UI/hook_failure_window.h"
#include "../UI/imgui_backend.h"
#include "../UI/imgui_host_interface.h"
#include "../UI/console_window.h"
#include "../UI/modloader_window.h"
#include "../UI/overlay.h"
#include "../UI/plugin_panel_registry.h"
#include "../UI/plugin_scan_window.h"
#include "../UI/plugin_widget_registry.h"
#include "../UI/splash_window.h"
#include "../UI/theme.h"
#include "../UI/tick_profiler_window.h"
#include "../UI/toast.h"
#include "../UI/update_notice_window.h"
#include "../UI/wheel_test.h"
#include "../logging/log.h"

static bool         s_imguiEnabled = true;
static EModKey      s_openKey      = EModKey::F2;

// The PAUSE_MENU and CURSOR_UP UI-state bits, read from the atomics the tick
// publishes them through (UIState::Publish is composed from the same two).
static bool CursorScreenUp()
{
    return Hooks::GameMenu::IsPauseMenuOpen() || Hooks::GameMenu::IsGameCursorShown();
}

bool ShouldCaptureInputNow()
{
    // Any standalone modloader window that can render independently of the main
    // F2 menu (i.e. can still be open while IsOpen() is false) must be listed
    // here, or its buttons become unclickable -- mouse messages are only
    // forwarded to ImGui while this returns true.
    const bool loaderWindowOpen = UI::ModLoaderWindow::IsOpen()
        || UI::UpdateNoticeWindow::IsOpen()
        || UI::PluginScanWindow::IsOpen()
        || UI::HookFailureWindow::IsOpen()
        || UI::TickProfilerWindow::IsOpen()
        || UI::ConsoleWindow::IsOpen();

    // A plugin's panel or input-capture token captures exclusively in gameplay,
    // but not on a cursor screen: there the game's own menu stays usable around
    // the plugin's windows (see ShouldShareGameMouseNow).
    const bool pluginCaptures = UI::PluginPanelRegistry::AnyPanelOpen()
        || UI::PluginPanelRegistry::AnyInputCaptureRequested();

    return InputArbitration::IsExclusive(loaderWindowOpen, pluginCaptures, CursorScreenUp());
}

bool ShouldPassthroughInputNow()
{
    // Cooperative mode -- only consulted while ShouldCaptureInputNow() is false.
    // A plugin driving an always-on widget UI (e.g. a timeline editor) holds a
    // passthrough token so its windows stay clickable without freezing the
    // player out of the game underneath.
    return UI::PluginPanelRegistry::AnyInputPassthroughRequested();
}

bool ShouldShareGameMouseNow()
{
    // The pause menu and the game's other cursor screens (machine menus, the
    // recipe station, the inventory) show the game's own cursor, and the ImGui
    // host feeds ImGui the mouse there unless exclusive capture owns it. Only
    // windows that opted in take it; everything else is drawn click-through,
    // so the screen under it keeps its clicks. A plugin panel is not one of
    // the exclusive owners here, so the clicks it does not cover go to the game.
    return !ShouldCaptureInputNow() && CursorScreenUp();
}

bool IsMenuWidgetUnderCursorNow()
{
    return Hooks::GameMenu::IsMenuWidgetHovered();
}

void InitClientUI()
{
    const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");
    Hooks::TextInputFocus::LoadConfig(iniPath.c_str());
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

        // A row in the game's own main menu and pause menu, sitting between
        // OPTIONS and CREDITS. The open key above is the fast way in once you
        // know it exists; this is how someone finds out that it does.
        //
        // Registered here, before the menus are ever built, and removable from
        // modloader.ini for anyone who would rather the game's menus stayed
        // untouched.
        int gameMenuEntry = GetPrivateProfileIntW(L"UI", L"GameMenuEntry", -1, iniPath.c_str());
        if (gameMenuEntry == -1)
        {
            // Written back so the key is visible in the ini rather than
            // being something you have to already know about to turn off.
            WritePrivateProfileStringW(L"UI", L"GameMenuEntry", L"1", iniPath.c_str());
            gameMenuEntry = 1;
        }
        if (gameMenuEntry != 0)
        {
            PluginGameMenuEntryDesc desc{};
            desc.id       = "modloader";
            desc.label    = "MOD LOADER";
            desc.targets  = PLUGIN_GAME_MENU_MAIN | PLUGIN_GAME_MENU_PAUSE;
            desc.anchor   = PLUGIN_GAME_MENU_ANCHOR_AFTER_OPTIONS;
            desc.onClick  = [](void*) { UI::ModLoaderWindow::Toggle(); };
            desc.userData = nullptr;
            GameMenu::Registry::AddLoaderEntry(&desc);
        }
        else
        {
            // Logged rather than left silent: "there is no MOD LOADER row" is a
            // question the log should answer without anyone opening the ini.
            LogToFile::Info(
                "[GameMenu] The mod loader's own menu row is disabled ([UI] GameMenuEntry=0)");
        }

        // Developer console -- opt-in, registers its own open key (default Tilde).
        UI::ConsoleWindow::Load(iniPath.c_str());

        ImGuiRenderCallbacks cbs{};
        cbs.RenderFrame = [](IModLoaderImGui* api)
        {
            // Applied here (not at startup) because the shared ImGui context
            // is only created lazily by the host DLL on the first render --
            // RenderFrame is guaranteed to fire after that.
            static bool s_themeApplied = false;
            if (!s_themeApplied)
            {
                // StartupLoadTheme always ends by applying some theme (the
                // saved one, a migrated one, or "Default"), and every theme
                // switch runs through ResetColors() -> Apply() first for a
                // clean baseline, so there's no separate Apply() to call here.
                UI::Theme::StartupLoadTheme(UI::GlobalSettings::GetIniPath());
                s_themeApplied = true;
            }

            // Keyboard navigation (Tab, arrows, Space/Enter) only while an
            // overlay window owns the keyboard. Left on at other times, a
            // focused plugin window would make ImGui claim every key
            // (io.WantCaptureKeyboard follows NavActive) and cooperative mode
            // would then cut WASD out of the game. Read at the next NewFrame.
            ImGuiIO& io = ImGui::GetIO();
            if (UI::GlobalSettings::GetKeyboardNavigation() && ShouldCaptureInputNow())
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            else
                io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

            UI::Overlay::Render();
            UI::Overlay::RenderHud();
            UI::ModLoaderWindow::Render(api);
            UI::UpdateNoticeWindow::Render();
            UI::PluginScanWindow::Render();
            UI::HookFailureWindow::Render();
            UI::TickProfilerWindow::Render();
            UI::ConsoleWindow::Render();
            UI::PluginPanelRegistry::RenderPanelWindows(api);
            UI::PluginWidgetRegistry::RenderWidgets(api);
            UI::Toast::Render();
#ifdef _DEBUG
            UI::WheelTest::RenderOverlay();
#endif
        };
        cbs.ShouldCaptureInput     = []() -> bool { return ShouldCaptureInputNow(); };
        cbs.ShouldPassthroughInput = []() -> bool { return ShouldPassthroughInputNow(); };
        cbs.DispatchKey = [](UINT msg, WPARAM wParam, LPARAM lParam) -> bool
        {
            return Hooks::Input::ProcessWindowMessage(msg, wParam, lParam);
        };
        cbs.DispatchMouseWheel = [](UINT msg, WPARAM wParam, LPARAM lParam, bool uiCapturing) -> bool
        {
            return Hooks::MouseWheel::Dispatch(msg, wParam, lParam, uiCapturing);
        };
        cbs.IsGameMouseShared = []() -> bool { return ShouldShareGameMouseNow(); };
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
        Hooks::SlatePresent::Install();
    }

    static auto s_onWorldReady = [](SDK::UWorld* /*world*/, const char* worldName)
    {
        const bool isMainMenu = ReloadPolicy::IsMainMenuWorldName(worldName);
        UI::Overlay::SetVisible(isMainMenu);
        UI::GlobalSettings::SetWorldName(worldName ? worldName : "");

        // What the auto-updater replaced this boot, and any plugin whose hooks
        // did not resolve. The failure one opens second so it takes focus -- it
        // is the one that needs acting on. The update notice is a genuine
        // one-shot; the hook failure window is only ARMED here and opens again
        // by itself whenever a later load or reload adds a report.
        if (isMainMenu)
        {
            UI::UpdateNoticeWindow::ShowIfPending();
            UI::HookFailureWindow::ShowIfPending();
        }
    };
    Hooks::WorldBeginPlay::RegisterAnyWorldCallback(s_onWorldReady);

    static auto s_onTick = [](float)
    {
        Hooks::GameMenu::Tick();

        // Look up the current world fresh every tick -- a cached pointer can
        // outlive the world it points to between EndPlay and the next
        // world's BeginPlay.
        SDK::UWorld* currentWorld = SdkGlobals::GetWorld();
        SDK::APlayerController* pc = currentWorld
            ? SDK::UGameplayStatics::GetPlayerController(currentWorld, 0) : nullptr;
        // Whether a game screen has the mouse cursor up, for the ImGui host's
        // mouse sharing (ShouldShareGameMouseNow).
        Hooks::GameMenu::SetGameCursorShown(pc && pc->bShowMouseCursor);

        // The plugins' UI state (IPluginGameMenu::GetUIState) as of this tick.
        // The menu layer is read only while a plugin wants it.
        uint64_t uiState = 0;
        if (ReloadPolicy::GameplayWorldRunning())  uiState |= PLUGIN_UI_IN_GAMEPLAY;
        if (Hooks::GameMenu::IsPauseMenuOpen())    uiState |= PLUGIN_UI_PAUSE_MENU;
        if (Hooks::GameMenu::IsGameCursorShown())  uiState |= PLUGIN_UI_CURSOR_UP;
        if (pc && UIState::MenuReadWanted())       uiState |= Hooks::GameMenu::ReadMenuLayer(pc);
        UIState::Publish(uiState);

        // Whether a game menu widget is under the cursor, for the widgets that
        // take the mouse only where none is. Read only while one is on screen;
        // true (click-through) when nobody asked or the layout check switched
        // the menu-layer read off.
        bool menuHovered = true;
        if (pc && CursorScreenUp() && !UIState::MenuReadDisabled() && UI::PluginWidgetRegistry::AnyWantsMenuHover())
            menuHovered = Hooks::GameMenu::ReadMenuHover(pc);
        Hooks::GameMenu::SetMenuWidgetHovered(menuHovered);

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
    {
        Hooks::SlatePresent::Remove();
        UI::ImGuiBackend::Shutdown();
    }
    Hooks::Input::RemoveInputProcessor();
}

#endif // MODLOADER_CLIENT_BUILD
