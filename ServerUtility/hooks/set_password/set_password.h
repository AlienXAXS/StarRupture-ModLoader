#pragma once

#include <cstdint>

// Hooks UCrDedicatedServerSettingsComp::SetPassword and
// UCrDedicatedServerSettingsComp::SetPlayerPassword to allow overriding
// server passwords via the -Password= and -PlayerPassword= command-line
// parameters.
namespace SetPasswordHook
{
    // Called once the engine is ready and the patterns have been located.
    // Either or both addresses may be 0 if the pattern was not found;
    // only the non-zero ones will be hooked.
    void Install(uintptr_t setPasswordAddress, uintptr_t setPlayerPasswordAddress);

    // Called on engine shutdown to cleanly remove the hooks.
    void Remove();
}
