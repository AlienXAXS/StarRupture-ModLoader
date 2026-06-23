#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace DwmapiProxy
{
    // Load the real dwmapi.dll from System32 and resolve all original exports.
    // Returns true on success.
    bool Initialize();

    // Free the real dwmapi.dll handle.
    void Shutdown();
}
