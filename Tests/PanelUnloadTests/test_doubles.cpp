// Stand-ins for the loader pieces plugin_panel_registry.cpp reaches out to, so
// the test runs as a plain console program with no game and no plugin DLLs.
#include "test_doubles.h"
#include "plugins/plugin_manager.h"
#include "logging/logger.h"
#include "theme.h"

namespace TestDoubles
{
    bool                     drawWindows = false;
    std::vector<std::string> drawnTitles;
    std::atomic<int>         loggedErrors{ 0 };
}

// FireConfigChanged resolves a plugin name through here; no test uses it.
namespace PluginManager
{
    const IPluginSelf* GetSelfForPlugin(const char*)
    {
        return nullptr;
    }
}

// The registries report a render that outlived the unload wait through this.
namespace ModLoaderLogger
{
    void LogError(const wchar_t*, ...)
    {
        ++TestDoubles::loggedErrors;
    }
}

// plugin_panel_registry.cpp draws panel windows through these; nothing here
// renders, but the panel's renderFn is called when drawWindows is set.
namespace UI::Theme
{
    bool BeginChamferedWindow(const char* title, const char*, bool*, const char*, ImGuiWindowFlags, bool)
    {
        TestDoubles::drawnTitles.push_back(title);
        return TestDoubles::drawWindows;
    }

    void EndChamferedWindow() {}
}
