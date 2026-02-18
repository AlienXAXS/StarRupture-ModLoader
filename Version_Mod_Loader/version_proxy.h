#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace VersionProxy
{
    // Load the real version.dll from System32 and resolve all original exports.
    // Returns true on success.
    bool Initialize();

    // Free the real version.dll handle.
    void Shutdown();
}
