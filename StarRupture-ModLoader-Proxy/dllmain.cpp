// dllmain.cpp : DLL entry point for the dwmapi.dll proxy shim.
//
// This DLL forwards real DWM exports (dwmapi_proxy.cpp) and bootstraps the
// real mod loader, StarRupture-ModLoader-Core.dll, located in the ModLoader\
// folder next to the game exe. The Core DLL is loaded dynamically (LoadLibrary
// + GetProcAddress, no static .lib link) so this proxy's own import table
// stays small.
#include "dwmapi_proxy.h"
#include "proxy_log.h"
#include "autoupdate.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "autoupdate_log.h"
#endif

typedef BOOL(*Core_Attach_t)();
typedef void(*Core_Detach_t)(BOOL processTerminating);

static HMODULE        g_coreModule     = nullptr;
static Core_Detach_t   g_coreDetachFn   = nullptr;

static bool LoadAndAttachCore()
{
    wchar_t corePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, corePath, MAX_PATH);
    wchar_t* sl = wcsrchr(corePath, L'\\');
    if (sl)
        wcscpy_s(sl + 1, MAX_PATH - static_cast<rsize_t>(sl + 1 - corePath),
            L"ModLoader\\StarRupture-ModLoader-Core.dll");

    if (GetFileAttributesW(corePath) == INVALID_FILE_ATTRIBUTES)
    {
        ProxyLog::Error("FATAL: ModLoader\\StarRupture-ModLoader-Core.dll not found");
        MessageBoxW(nullptr,
            L"StarRupture-ModLoader-Core.dll is missing from the ModLoader folder.\n\n"
            L"Please reinstall the mod loader.",
            L"StarRupture Mod Loader -- Missing File",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }

    g_coreModule = LoadLibraryW(corePath);
    if (!g_coreModule)
    {
        ProxyLog::Error("FATAL: LoadLibraryW failed for Core DLL (error %lu)", GetLastError());
        MessageBoxW(nullptr,
            L"Failed to load StarRupture-ModLoader-Core.dll.\n\n"
            L"Please reinstall the mod loader.",
            L"StarRupture Mod Loader -- Load Failed",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }

    auto attachFn = reinterpret_cast<Core_Attach_t>(GetProcAddress(g_coreModule, "Core_Attach"));
    g_coreDetachFn = reinterpret_cast<Core_Detach_t>(GetProcAddress(g_coreModule, "Core_Detach"));
    if (!attachFn || !g_coreDetachFn)
    {
        ProxyLog::Error("FATAL: Core_Attach/Core_Detach not exported by Core DLL");
        FreeLibrary(g_coreModule);
        g_coreModule = nullptr;
        return false;
    }

    if (!attachFn())
    {
        ProxyLog::Error("FATAL: Core_Attach() returned failure");
        FreeLibrary(g_coreModule);
        g_coreModule = nullptr;
        g_coreDetachFn = nullptr;
        return false;
    }

    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        // Register ModLoader\ as an extra DLL search directory so that
        // LoadLibraryW("ModLoader\\StarRupture-ModLoader-Core.dll") below (and
        // Core's own delay-loaded StarRupture-ImGui.dll) can find their
        // dependencies there.
        {
            wchar_t modloaderPath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modloaderPath, MAX_PATH);
            wchar_t* sl = wcsrchr(modloaderPath, L'\\');
            if (sl)
                wcscpy_s(sl + 1, MAX_PATH - static_cast<rsize_t>(sl + 1 - modloaderPath), L"ModLoader");
            SetDllDirectoryW(modloaderPath);
        }

        ProxyLog::Info("StarRupture Mod Loader (dwmapi.dll proxy) loaded");

        ProxyLog::Info("Initializing dwmapi.dll proxy...");
        if (!DwmapiProxy::Initialize())
        {
            ProxyLog::Error("FATAL: Failed to initialize dwmapi proxy -- DLL load aborted");
            return FALSE;
        }
        ProxyLog::Info("Dwmapi proxy initialized successfully");

#if defined(MODLOADER_CLIENT_BUILD)
        // Self-update (client only).  Applying a staged update MUST happen
        // before the Core DLL is loaded (its file would be locked afterwards)
        // and uses file operations only, which are safe under loader lock.
        AutoUpdateLog::Initialize();
        ProxyAutoUpdate::ApplyPendingUpdate();
#endif

        if (!LoadAndAttachCore())
        {
            DwmapiProxy::Shutdown();
            return FALSE;
        }

#if defined(MODLOADER_CLIENT_BUILD)
        // Network check runs on its own thread once the loader lock is
        // released; if the user accepts, the update is staged and applied
        // at the start of the next boot (see autoupdate.h).
        ProxyAutoUpdate::StartBackgroundCheck();
#endif
    }
    break;

    case DLL_PROCESS_DETACH:
    {
        if (g_coreDetachFn)
            g_coreDetachFn(lpReserved != nullptr);

        if (lpReserved != nullptr)
            break; // process terminating -- skip the rest to avoid loader-lock issues

        if (g_coreModule)
        {
            FreeLibrary(g_coreModule);
            g_coreModule = nullptr;
        }

        DwmapiProxy::Shutdown();

#if defined(MODLOADER_CLIENT_BUILD)
        AutoUpdateLog::Shutdown();
#endif
    }
    break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
