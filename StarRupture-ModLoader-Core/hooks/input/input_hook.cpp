#include "pch.h"
#include "input_hook.h"

// Client-only feature.
#ifdef MODLOADER_CLIENT_BUILD

#include "keybind_registry.h"
#include "hooks/hooks_common.h"
#include "hooks/game/scan_patterns.h"
#include "memory_scanner/scanner.h"
#include "logging/logger.h"
#include "UI/plugin_panel_registry.h"
#include "core/client_ui.h"
#include "core/input_arbitration.h"
#include "core/sdk_globals.h"
#include "memory_scanner/version_map.h"
#include "hooks/game/version_map.generated.h"

#include "Basic.hpp"

#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <string>

namespace Hooks::InputHook
{
    // -----------------------------------------------------------------------
    // UGameViewportClient::InputKey signature
    // __int64 __fastcall InputKey(UGameViewportClient* This,
    //                             const FInputKeyEventArgs* InEventArgs)
    // Returns false (0) to consume the input; true (non-zero) to pass it on.
    // -----------------------------------------------------------------------
    using InputKey_t = __int64(__fastcall*)(void* This, const void* InEventArgs);

    static Hook          g_hook;
    static InputKey_t    g_original = nullptr;

    // -----------------------------------------------------------------------
    // FInputKeyEventArgs field offsets (UE5)
    //   +0x00  FViewport*    Viewport
    //   +0x08  int32         ControllerId
    //   +0x0C  uint8[4]      padding
    //   +0x10  FKey          Key  <- FName at +0x10: ComparisonIndex, Number (uint32 each)
    //   +0x28  EInputEvent   Event  (0=Pressed, 1=Released, 2=Repeat)
    //   +0x2C  float         AmountDepressed
    //   +0x30  bool          bGamepad
    // -----------------------------------------------------------------------
    static constexpr int ARGS_FNAME_IDX_OFFSET = 0x10;  // uint32
    static constexpr int ARGS_FNAME_NUM_OFFSET = 0x14;  // uint32
    static constexpr int ARGS_EVENT_OFFSET      = 0x28;  // int32 / enum

    enum class EInputEvent : int32_t
    {
        Pressed  = 0,
        Released = 1,
        Repeat   = 2,
    };

    // -----------------------------------------------------------------------
    // FName -> EModKey
    // Dispatch already happened in HookedWndProc (covers all game states).
    // This hook only needs to resolve the key in order to check ShouldBlock
    // and return false (0) to consume the input from the UE5 input stack.
    //
    // The key comes from the event's own FName: its text ("LeftControl", "F5")
    // is looked up in the keybind registry's key names. A name with no mod-key
    // equivalent (MouseScrollUp, MouseScrollDown, gamepad buttons and axes)
    // resolves to EModKey::Unknown and is never blocked. An FName's text never
    // changes, so each result is cached for the session.
    // -----------------------------------------------------------------------
    static std::unordered_map<uint64_t, EModKey> s_fnameCache;
    static std::mutex                            s_fnameMutex;

    // Keys (FName indices) whose Pressed event was handed to the game.
    static InputArbitration::PressesSeenByGame   s_pressesSeen;

    // FName::ToString calls the game's FName::AppendString: the SDK's own
    // address on the build it was made for, the resolved one elsewhere.
    static bool FNameTextAvailable()
    {
        return VersionMap::RunningExe().cl == VersionMap::kBuiltCL || SdkGlobals::SdkOnResolvedGlobals();
    }

    static EModKey KeyForFName(uint32_t fnameIdx, uint32_t fnameNumber)
    {
        const uint64_t id = (static_cast<uint64_t>(fnameNumber) << 32) | fnameIdx;
        {
            std::lock_guard<std::mutex> lock(s_fnameMutex);
            auto it = s_fnameCache.find(id);
            if (it != s_fnameCache.end())
                return it->second;
        }

        if (!FNameTextAvailable())
            return EModKey::Unknown;

        const std::string name = SDK::FName(static_cast<int32_t>(fnameIdx), fnameNumber).ToString();
        const EModKey mk = Hooks::Input::NameToModKey(name.c_str());
        ModLoaderLogger::LogDebug(L"[InputHook] Key FName %u/%u is '%S' (%S)", fnameIdx, fnameNumber,
                                  name.c_str(), mk == EModKey::Unknown ? "no mod key" : Hooks::Input::ModKeyToName(mk));

        std::lock_guard<std::mutex> lock(s_fnameMutex);
        s_fnameCache[id] = mk;
        return mk;
    }

    // -----------------------------------------------------------------------
    // Detour -- blocking only
    //
    // Keybind callbacks are dispatched in HookedWndProc which fires for every
    // key message in all game states (main menu, loading screen, in-game).
    // This detour's sole job is to return false (0) when the key is marked
    // as blocking, consuming it from UE5's input stack.
    // -----------------------------------------------------------------------
    static __int64 __fastcall Detour(void* This, const void* InEventArgs)
    {
        // While any plugin holds an input-capture token, the game must not
        // see input at all (any event type -- Pressed/Released/Repeat -- so
        // we don't leave the game with a stuck "key down" state from a
        // swallowed Pressed event). Not on a cursor screen: there the token
        // shares the mouse and keyboard (ShouldCaptureInputNow), and the
        // window procedure already keeps keys from the game while a text
        // field has focus. A release for a press the game saw still goes
        // through: capture can start in between.
        const bool capturing = UI::PluginPanelRegistry::AnyInputCaptureRequested() && ShouldCaptureInputNow();

        if (!InEventArgs)
            return capturing ? 0 : (g_original ? g_original(This, InEventArgs) : 0);

        const uint8_t* args = static_cast<const uint8_t*>(InEventArgs);

        EInputEvent ueEvent = *reinterpret_cast<const EInputEvent*>(args + ARGS_EVENT_OFFSET);
        uint32_t fnameIdx = *reinterpret_cast<const uint32_t*>(args + ARGS_FNAME_IDX_OFFSET);
        uint32_t fnameNum = *reinterpret_cast<const uint32_t*>(args + ARGS_FNAME_NUM_OFFSET);

        if (ueEvent == EInputEvent::Released &&
            !InputArbitration::ShouldKeepFromGame(capturing, s_pressesSeen.TakeRelease(fnameIdx)))
            return g_original ? g_original(This, InEventArgs) : 0;

        if (capturing)
            return 0;

        // Only act on Pressed edges -- that is all we need for blocking. Repeat
        // and double-click events that go through still mean the key is down.
        if (ueEvent != EInputEvent::Pressed)
        {
            s_pressesSeen.Pressed(fnameIdx);
            return g_original ? g_original(This, InEventArgs) : 0;
        }

        const EModKey mk = KeyForFName(fnameIdx, fnameNum);
        if (mk != EModKey::Unknown &&
            Hooks::Input::ShouldBlock(mk, Hooks::Input::SampleCurrentModifiers()))
            return 0; // consumed -- UE5 never processes this key

        s_pressesSeen.Pressed(fnameIdx);
        return g_original ? g_original(This, InEventArgs) : 0;
    }

    // -----------------------------------------------------------------------
    // Install / Remove
    // -----------------------------------------------------------------------
    void Install()
    {
        ModLoaderLogger::LogInfo(L"[InputHook] Installing UGameViewportClient::InputKey hook...");

        uintptr_t addr = Scanner::FindPatternInMainModule(
            "UGameViewportClient::InputKey",
            ScanPatterns::UGameViewportClient_InputKey);

        if (!addr)
        {
            ModLoaderLogger::LogError(L"[InputHook] Pattern scan failed -- InputKey hook not installed");
            return;
        }

        ModLoaderLogger::LogDebug(L"[InputHook] Found InputKey at 0x%llX", (unsigned long long)addr);

        void* originalOut = nullptr;
        if (!g_hook.Install(addr, reinterpret_cast<void*>(&Detour), &originalOut))
        {
            ModLoaderLogger::LogError(L"[InputHook] Hook install failed");
            return;
        }

        g_original = reinterpret_cast<InputKey_t>(originalOut);
        ModLoaderLogger::LogInfo(L"[InputHook] UGameViewportClient::InputKey hook installed");
    }

    void Remove()
    {
        g_hook.Remove();
        g_original = nullptr;

        {
            std::lock_guard<std::mutex> lock(s_fnameMutex);
            s_fnameCache.clear();
        }

        ModLoaderLogger::LogInfo(L"[InputHook] UGameViewportClient::InputKey hook removed");
    }

} // namespace Hooks::InputHook

#endif // MODLOADER_CLIENT_BUILD
