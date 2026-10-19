// Stand-ins for the loader pieces ConfigEdit and the callback registry sit on,
// so the tests run as a plain console program with no game and no plugin DLLs.
//
// The code under test is compiled from the loader source as it is:
//   config/config_edit.cpp, config/config_manager.cpp, UI/plugin_panel_registry.cpp
// Only what those reach outward to is replaced here.
#include "test_doubles.h"

#include "plugins/plugin_manager.h"
#include "hooks/input/keybind_registry.h"
#include "logging/logger.h"
#include "theme.h"

#include <cstring>

namespace TestDoubles
{
    std::vector<BlockingCall> blockingCalls;
    std::vector<RebindCall>   rebindCalls;

    static const IPluginSelf* s_selves[2] = {};

    void ResetRecordedCalls()
    {
        blockingCalls.clear();
        rebindCalls.clear();
    }

    void SetPluginSelves(const IPluginSelf* a, const IPluginSelf* b)
    {
        s_selves[0] = a;
        s_selves[1] = b;
    }
}

// config_manager.cpp logs through these; the tests do not need the output.
namespace ModLoaderLogger
{
    void LogDebug(const wchar_t*, ...) {}
    void LogInfo(const wchar_t*, ...) {}
    void LogWarn(const wchar_t*, ...) {}
    void LogError(const wchar_t*, ...) {}
}

namespace Hooks::Input
{
    void SetComboBlocking(const char* owner, const char* comboStr, bool blocking)
    {
        TestDoubles::blockingCalls.push_back({ owner, comboStr, blocking });
    }

    void UpdateKeybindByName(const char* pluginName, const char* oldCombo, const char* newCombo)
    {
        TestDoubles::rebindCalls.push_back({ pluginName, oldCombo, newCombo });
    }
}

// FireConfigChanged resolves the changed plugin to its self pointer through here.
namespace PluginManager
{
    const IPluginSelf* GetSelfForPlugin(const char* pluginName)
    {
        for (const IPluginSelf* s : TestDoubles::s_selves)
            if (s && pluginName && _stricmp(s->name, pluginName) == 0)
                return s;
        return nullptr;
    }
}

// plugin_panel_registry.cpp draws panel windows through these; nothing here
// renders.
namespace UI::Theme
{
    bool BeginChamferedWindow(const char*, const char*, bool*, const char*, ImGuiWindowFlags, bool)
    {
        return false;
    }

    void EndChamferedWindow() {}
}
