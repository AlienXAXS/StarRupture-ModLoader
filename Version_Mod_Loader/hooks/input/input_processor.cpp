#include "pch.h"
#include "input_processor.h"
#include "keybind_registry.h"
#include "hooks/game/engine_tick/engine_tick.h"
#include "logging/logger.h"

#include <windows.h>
#include <map>

// Client-only feature — entire translation unit is a no-op on server/generic builds.
#ifdef MODLOADER_CLIENT_BUILD

namespace Hooks::Input
{
    // -----------------------------------------------------------------------
    // Per-key state tracking
    // -----------------------------------------------------------------------
    // Maps VK code -> true if the key was down at the end of the previous tick.
    static std::map<int, bool> s_prevKeyState;

    // -----------------------------------------------------------------------
    // Per-frame poll callback — registered into the engine tick
    // -----------------------------------------------------------------------
    static void OnEngineTick(float /*deltaSeconds*/)
    {
        // Retrieve the set of keys that have at least one registered callback.
        // This is rebuilt each tick so newly registered keys are picked up immediately.
        auto activeKeys = GetActiveKeys();

        for (auto& kv : activeKeys)
        {
            EModKey key = kv.first;
            int     vk  = kv.second;

            // GetAsyncKeyState: high bit set = key is currently pressed.
            bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool wasDown = false;

            auto it = s_prevKeyState.find(vk);
            if (it != s_prevKeyState.end())
                wasDown = it->second;
            else
                s_prevKeyState[vk] = false;

            if (isDown && !wasDown)
            {
                // Key just went down — fire Pressed callbacks
                Dispatch(key, EModKeyEvent::Pressed);
            }
            else if (!isDown && wasDown)
            {
                // Key just came up — fire Released callbacks
                Dispatch(key, EModKeyEvent::Released);
            }

            s_prevKeyState[vk] = isDown;
        }
    }

    // -----------------------------------------------------------------------
    // Install / Remove
    // -----------------------------------------------------------------------
    void InstallInputProcessor()
    {
        Initialize(); // ensure registry tables are ready

        Hooks::EngineTick::RegisterPluginCallback(OnEngineTick);
        ModLoaderLogger::LogInfo(L"[InputProcessor] Engine tick callback registered (keybind polling active)");
    }

    void RemoveInputProcessor()
    {
        Hooks::EngineTick::UnregisterPluginCallback(OnEngineTick);
        s_prevKeyState.clear();
        Shutdown(); // clear all registered keybinds
        ModLoaderLogger::LogInfo(L"[InputProcessor] Engine tick callback unregistered");
    }

} // namespace Hooks::Input

#endif // MODLOADER_CLIENT_BUILD
