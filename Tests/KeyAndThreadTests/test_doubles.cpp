// Stand-ins for what keybind_registry.cpp reaches out to, so the test runs as
// a plain console program with no game, no overlay and no plugin DLLs.
#include "test_doubles.h"
#include "plugins/plugin_manager.h"
#include "logging/logger.h"

namespace TestDoubles
{
    namespace
    {
        std::atomic<bool> g_down[256] = {};
    }

    std::atomic<HWND> foreground{ nullptr };
    std::atomic<HWND> gameWindow{ nullptr };
    std::atomic<bool> buttonsSwapped{ false };
    std::atomic<bool> gameTextFocused{ false };
    std::atomic<bool> imguiTextActive{ false };
    std::atomic<long> gameTextChecks{ 0 };

    void SetKeyDown(int vk, bool down)
    {
        if (vk > 0 && vk < 256)
            g_down[vk].store(down);
    }

    void ReleaseAllKeys()
    {
        for (auto& key : g_down)
            key.store(false);
    }

    // The high bit, as GetAsyncKeyState reports a key that is down.
    SHORT AsyncKeyState(int vk)
    {
        return (vk > 0 && vk < 256 && g_down[vk].load()) ? static_cast<SHORT>(0x8000) : 0;
    }

    HWND ForegroundWindow()
    {
        return foreground.load();
    }

    int SystemMetrics(int index)
    {
        return (index == SM_SWAPBUTTON && buttonsSwapped.load()) ? 1 : 0;
    }
}

namespace UI::ImGuiBackend
{
    HWND GetGameWindow()     { return TestDoubles::gameWindow.load(); }
    bool IsTextInputActive() { return TestDoubles::imguiTextActive.load(); }
}

namespace Hooks::TextInputFocus
{
    bool IsGameTextInputFocused()
    {
        ++TestDoubles::gameTextChecks;
        return TestDoubles::gameTextFocused.load();
    }
}

// UpdateKeybindByName resolves a plugin through here; no test rebinds.
namespace PluginManager
{
    HMODULE GetPluginModule(const char*) { return nullptr; }
}

namespace ModLoaderLogger
{
    void LogDebug(const wchar_t*, ...) {}
    void LogInfo(const wchar_t*, ...)  {}
    void LogWarn(const wchar_t*, ...)  {}
    void LogError(const wchar_t*, ...) {}
}
