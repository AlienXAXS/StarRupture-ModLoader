#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// FHttpConnection::ProcessRequest hook
//
// Intercepts calls to FHttpConnection::ProcessRequest and logs the values
// of all three parameters passed to the function.  After logging, the
// original function is called so normal behaviour is preserved.
// ---------------------------------------------------------------------------

namespace HttpConnectionHook
{
    // Scan for FHttpConnection::ProcessRequest and install the detour.
    // Call once after the engine is ready.
    void Install();

    // Remove the detour and restore the original function.
    void Remove();
}
