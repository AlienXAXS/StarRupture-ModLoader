#pragma once
#include <windows.h>

// ---------------------------------------------------------------------------
// ABI boundary between the dwmapi.dll proxy shim (StarRupture-ModLoader-Proxy) and
// this DLL (StarRupture-ModLoader-Core.dll).
//
// The proxy resolves these by name via GetProcAddress after LoadLibrary --
// there is no static .lib link between the two projects, so the proxy's
// import table stays minimal.
// ---------------------------------------------------------------------------

extern "C"
{
    // Called once, synchronously, from the proxy's DllMain(DLL_PROCESS_ATTACH)
    // after DwmapiProxy::Initialize() has already succeeded. Performs legacy
    // file migration, the ImGui-presence check (client builds), creates the
    // sync events, and dispatches MainInitThreadProc via APC or a new thread.
    // Returns FALSE on unrecoverable failure (proxy should then fail DllMain).
    __declspec(dllexport) BOOL Core_Attach();

    // Called once from the proxy's DllMain(DLL_PROCESS_DETACH), before the
    // proxy shuts down DwmapiProxy/LogToFile. No-op if Core_Attach was never
    // called or already failed. `processTerminating` mirrors lpReserved !=
    // nullptr -- when true, shutdown is skipped to avoid loader-lock issues
    // during process exit, matching the previous in-DllMain behavior.
    __declspec(dllexport) void Core_Detach(BOOL processTerminating);
}
