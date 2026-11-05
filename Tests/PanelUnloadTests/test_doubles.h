#pragma once

#include <atomic>
#include <string>
#include <vector>

// What the test doubles in test_doubles.cpp do and record, so the render tests
// can draw panels without a game and see what was drawn and logged.
namespace TestDoubles
{
    // BeginChamferedWindow returns this, so renderFn runs only when it is true.
    extern bool drawWindows;

    // Titles BeginChamferedWindow was called with, in order.
    extern std::vector<std::string> drawnTitles;

    // The window flags, and whether a title-bar callback came with it, for each
    // of those calls. The callback is run, as DrawTitleBar would.
    extern std::vector<int>  drawnFlags;
    extern std::vector<bool> drawnWithTitleBar;

    // The interface version PluginManager says every module's plugin declared.
    extern int declaredInterface;

    // ModLoaderLogger::LogError calls.
    extern std::atomic<int> loggedErrors;
}
