#pragma once

#include "plugins/plugin_interface.h"
#include <string>
#include <vector>

// What the test doubles in test_doubles.cpp recorded, so a test can check the
// calls ConfigEdit made into the keybind registry.
namespace TestDoubles
{
    struct BlockingCall
    {
        std::string owner;
        std::string combo;
        bool        blocking;
    };

    struct RebindCall
    {
        std::string plugin;
        std::string oldCombo;
        std::string newCombo;
    };

    extern std::vector<BlockingCall> blockingCalls;
    extern std::vector<RebindCall>   rebindCalls;

    void ResetRecordedCalls();

    // The plugins PluginManager::GetSelfForPlugin resolves by name.
    void SetPluginSelves(const IPluginSelf* a, const IPluginSelf* b = nullptr);
}
