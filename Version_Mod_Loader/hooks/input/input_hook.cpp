#include "pch.h"
#include "input_hook.h"

// Client-only feature.
#ifdef MODLOADER_CLIENT_BUILD

#include "keybind_registry.h"
#include "hooks/hooks_common.h"
#include "hooks/game/scan_patterns.h"
#include "memory_scanner/scanner.h"
#include "UI/modloader_window.h"
#include "logging/logger.h"

#include <windows.h>
#include <unordered_map>
#include <mutex>

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
    //   +0x10  FKey          Key  <- FName ComparisonIndex (uint32) at +0x10
    //   +0x28  EInputEvent   Event  (0=Pressed, 1=Released, 2=Repeat)
    //   +0x2C  float         AmountDepressed
    //   +0x30  bool          bGamepad
    // -----------------------------------------------------------------------
    static constexpr int ARGS_FNAME_IDX_OFFSET = 0x10;  // uint32
    static constexpr int ARGS_EVENT_OFFSET      = 0x28;  // int32 / enum

    enum class EInputEvent : int32_t
    {
        Pressed  = 0,
        Released = 1,
        Repeat   = 2,
    };

    // -----------------------------------------------------------------------
    // FName -> EModKey lazy cache
    // On Pressed events we scan GetAsyncKeyState to find which VK is down,
    // map it to an EModKey, and cache the FName ComparisonIndex.
    // On Released events the key is already up so we rely on the cache.
    // -----------------------------------------------------------------------
    static std::unordered_map<uint32_t, EModKey> s_fnameCache;
    static std::mutex                            s_fnameMutex;

    static EModKey LookupFNameCache(uint32_t fnameIdx)
    {
        std::lock_guard<std::mutex> lock(s_fnameMutex);
        auto it = s_fnameCache.find(fnameIdx);
        if (it != s_fnameCache.end())
            return it->second;
        return EModKey::Unknown;
    }

    static void CacheFName(uint32_t fnameIdx, EModKey mk)
    {
        std::lock_guard<std::mutex> lock(s_fnameMutex);
        s_fnameCache[fnameIdx] = mk;
    }

    // Scan the VK table for a key that is currently physically held.
    // Called only on Pressed events (key still down when hook fires).
    static EModKey ScanVKTableForDownKey()
    {
        auto activeKeys = Hooks::Input::GetActiveKeys();
        for (auto& kv : activeKeys)
        {
            if (GetAsyncKeyState(kv.second) & 0x8000)
                return kv.first;
        }
        return EModKey::Unknown;
    }

    // Returns true if the VK code belongs to a modifier key.
    static bool IsModifierVK(int vk)
    {
        return vk == VK_LCONTROL || vk == VK_RCONTROL ||
               vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
               vk == VK_LMENU    || vk == VK_RMENU;
    }

    // -----------------------------------------------------------------------
    // Detour
    // -----------------------------------------------------------------------
    static __int64 __fastcall Detour(void* This, const void* InEventArgs)
    {
        if (!InEventArgs)
            return g_original ? g_original(This, InEventArgs) : 0;

        const uint8_t* args = static_cast<const uint8_t*>(InEventArgs);

        EInputEvent ueEvent = *reinterpret_cast<const EInputEvent*>(args + ARGS_EVENT_OFFSET);

        // Ignore key repeat events entirely -- we only care about edges.
        if (ueEvent == EInputEvent::Repeat)
            return g_original ? g_original(This, InEventArgs) : 0;

        EModKeyEvent modEvent = (ueEvent == EInputEvent::Pressed)
                                    ? EModKeyEvent::Pressed
                                    : EModKeyEvent::Released;

        uint32_t fnameIdx = *reinterpret_cast<const uint32_t*>(args + ARGS_FNAME_IDX_OFFSET);

        // Resolve FName index to EModKey.
        EModKey mk = LookupFNameCache(fnameIdx);
        if (mk == EModKey::Unknown)
        {
            if (modEvent == EModKeyEvent::Pressed)
            {
                mk = ScanVKTableForDownKey();
                if (mk != EModKey::Unknown)
                    CacheFName(fnameIdx, mk);
            }
            else
            {
                // Released for an unknown key — just pass through.
                return g_original ? g_original(This, InEventArgs) : 0;
            }
        }

        if (mk == EModKey::Unknown)
            return g_original ? g_original(This, InEventArgs) : 0;

        // Do not dispatch modifier keys (Ctrl/Shift/Alt) as standalone keybinds.
        int vk = Hooks::Input::ModKeyToVK(mk);
        if (IsModifierVK(vk))
            return g_original ? g_original(This, InEventArgs) : 0;

        // Sample current modifier mask.
        EModKeyModifiers mods = Hooks::Input::SampleCurrentModifiers();

        // While the modloader UI is open, suppress all plugin keybind dispatch
        // to prevent accidental keybind fires while interacting with the UI.
        if (!UI::ModLoaderWindow::IsOpen())
        {
            Hooks::Input::Dispatch(mk, modEvent);
            Hooks::Input::DispatchCombo(mk, mods, modEvent);

            if (Hooks::Input::ShouldBlock(mk, mods))
            {
                // Return false (0) -- UE5 treats that as "input consumed".
                return 0;
            }
        }

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
