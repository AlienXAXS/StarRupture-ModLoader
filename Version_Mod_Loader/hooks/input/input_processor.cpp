#include "pch.h"
#include "input_processor.h"
#include "keybind_registry.h"
#include "logging/logger.h"

// Client-only feature -- entire translation unit is a no-op on server/generic builds.
#ifdef MODLOADER_CLIENT_BUILD

namespace Hooks::Input
{
    // -----------------------------------------------------------------------
    // Install / Remove
    //
    // Key dispatch now flows through the UGameViewportClient::InputKey hook
    // (input_hook.cpp) rather than a GetAsyncKeyState polling loop on the
    // engine tick.  These stubs retain the same public interface so call sites
    // elsewhere in the codebase (hooks_interface, imgui_backend) do not change.
    // -----------------------------------------------------------------------
    void InstallInputProcessor()
    {
        Initialize(); // ensure registry tables are ready
        ModLoaderLogger::LogInfo(L"[InputProcessor] Keybind registry initialised (dispatch via InputKey hook)");
    }

    void RemoveInputProcessor()
    {
        Shutdown(); // clear all registered keybinds
        ModLoaderLogger::LogInfo(L"[InputProcessor] Keybind registry shut down");
    }

} // namespace Hooks::Input

#endif // MODLOADER_CLIENT_BUILD
