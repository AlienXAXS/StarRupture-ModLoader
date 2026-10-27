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

    // ModLoaderLogger::LogError calls.
    extern std::atomic<int> loggedErrors;
}
