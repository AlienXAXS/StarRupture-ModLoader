#pragma once

#include <cstdint>

// Installs/removes the hook on UCrDedicatedServerSettingsComp::ParseSettings.
// When all required command-line parameters are present the hook returns 1
// (success) immediately and writes the parsed values directly into the
// component, bypassing the DSSettings.txt code path entirely.
namespace ParseSettingsHook
{
    // Called once the engine is ready and the pattern has been located.
    void Install(uintptr_t targetAddress);

    // Called on engine shutdown to cleanly remove the hook.
    void Remove();
}
