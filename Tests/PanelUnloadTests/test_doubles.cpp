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
    std::vector<int>         drawnFlags;
    std::vector<bool>        drawnWithTitleBar;
    int                      declaredInterface = 75;
    std::atomic<int>         loggedErrors{ 0 };
}

// FireConfigChanged resolves a plugin name through here; no test uses it.
namespace PluginManager
{
    const IPluginSelf* GetSelfForPlugin(const char*)
    {
        return nullptr;
    }

    // RegisterPanel asks which interface the owning plugin declared.
    int GetInterfaceVersionForModule(HMODULE)
    {
        return TestDoubles::declaredInterface;
    }
}

// The registries report a render that outlived the unload wait through this.
// The watchdog warns through LogWarn, which no test reaches.
namespace ModLoaderLogger
{
    void LogError(const wchar_t*, ...)
    {
        ++TestDoubles::loggedErrors;
    }

    void LogWarn(const wchar_t*, ...)
    {
    }
}

// plugin_panel_registry.cpp draws panel windows through these; nothing here
// renders, but the panel's renderFn is called when drawWindows is set.
namespace UI::Theme
{
    bool BeginChamferedWindow(const char* title, const char*, bool*, const char*, ImGuiWindowFlags flags, bool,
                              void (*besideTitle)())
    {
        TestDoubles::drawnTitles.push_back(title);
        TestDoubles::drawnFlags.push_back(flags);
        TestDoubles::drawnWithTitleBar.push_back(besideTitle != nullptr);
        if (TestDoubles::drawWindows && besideTitle)
            besideTitle();
        return TestDoubles::drawWindows;
    }

    void EndChamferedWindow() {}
}

// plugin_widget_registry.cpp asks this whether widgets share the pause
// menu's mouse; no test opens the pause menu.
bool ShouldShareGameMouseNow()
{
    return false;
}
