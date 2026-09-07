// dllmain.cpp : DLL entry point for the dwmapi.dll proxy shim.
//
// This DLL forwards real DWM exports (dwmapi_proxy.cpp) and bootstraps the
// real mod loader, StarRupture-ModLoader-Core.dll, located in the ModLoader\
// folder next to the game exe. The Core DLL is loaded dynamically (LoadLibrary
// + GetProcAddress, no static .lib link) so this proxy's own import table
// stays small.
#include "dwmapi_proxy.h"
#include "proxy_log.h"
#include <cstdio>   // swprintf_s
#include "updater_launch.h"
#if defined(MODLOADER_CLIENT_BUILD)
#include "autoupdate_log.h"
#endif

typedef BOOL(*Core_Attach_t)();
typedef void(*Core_Detach_t)(BOOL processTerminating);

static HMODULE        g_coreModule     = nullptr;
static Core_Detach_t   g_coreDetachFn   = nullptr;

// Turn the LoadLibrary error codes users actually hit into the sentence that
// tells them what to do. A bare "error 126" in a bug report costs a round trip
// every single time; naming the cause in the log does not.
static const char* DescribeLoadError(DWORD err)
{
    switch (err)
    {
    case ERROR_MOD_NOT_FOUND:       // 126
        return "a DLL the Core DLL depends on is missing -- most often the "
               "Visual C++ 2015-2022 x64 redistributable, or StarRupture-ImGui.dll";
    case ERROR_BAD_EXE_FORMAT:      // 193
        return "the DLL is not a valid x64 image -- a 32-bit or corrupt/truncated download";
    case ERROR_ACCESS_DENIED:       // 5
        return "access denied -- antivirus quarantine, or the file is blocked "
               "(right-click -> Properties -> Unblock)";
    case ERROR_FILE_NOT_FOUND:      // 2
        return "file not found (a dependency path, since the Core DLL itself exists)";
    case ERROR_PROC_NOT_FOUND:      // 127
        return "a dependency is present but the wrong version (missing export)";
    case ERROR_INVALID_IMAGE_HASH:  // 577
        return "the image failed signature validation -- blocked by a code-integrity policy";
    default:
        return "see the Windows system error code for details";
    }
}

static bool LoadAndAttachCore()
{
    wchar_t corePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, corePath, MAX_PATH);
    wchar_t* sl = wcsrchr(corePath, L'\\');
    if (sl)
        wcscpy_s(sl + 1, MAX_PATH - static_cast<rsize_t>(sl + 1 - corePath),
            L"ModLoader\\StarRupture-ModLoader-Core.dll");

    ProxyLog::Info("Core DLL path: %ls", corePath);

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

    ProxyLog::Debug("Calling LoadLibraryW on Core DLL...");
    g_coreModule = LoadLibraryW(corePath);
    if (!g_coreModule)
    {
        DWORD err = GetLastError();
        ProxyLog::Error("FATAL: LoadLibraryW failed for Core DLL (error %lu)", err);
        ProxyLog::Error("       likely cause: %s", DescribeLoadError(err));
        MessageBoxW(nullptr,
            L"Failed to load StarRupture-ModLoader-Core.dll.\n\n"
            L"Please reinstall the mod loader.",
            L"StarRupture Mod Loader -- Load Failed",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }

    ProxyLog::Info("Core DLL loaded at 0x%llX",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_coreModule)));

    auto attachFn = reinterpret_cast<Core_Attach_t>(GetProcAddress(g_coreModule, "Core_Attach"));
    g_coreDetachFn = reinterpret_cast<Core_Detach_t>(GetProcAddress(g_coreModule, "Core_Detach"));
    if (!attachFn || !g_coreDetachFn)
    {
        ProxyLog::Error("FATAL: Core_Attach/Core_Detach not exported by Core DLL "
                        "(Core_Attach=%p, Core_Detach=%p) -- mismatched Core DLL version",
                        reinterpret_cast<void*>(attachFn), reinterpret_cast<void*>(g_coreDetachFn));
        FreeLibrary(g_coreModule);
        g_coreModule = nullptr;
        return false;
    }

    ProxyLog::Debug("Core_Attach resolved at %p, Core_Detach at %p",
                    reinterpret_cast<void*>(attachFn), reinterpret_cast<void*>(g_coreDetachFn));

    ProxyLog::Info("Calling Core_Attach()...");
    if (!attachFn())
    {
        ProxyLog::Error("FATAL: Core_Attach() returned failure -- "
                        "see ModLoader\\Logs\\modloader.log for the reason");
        FreeLibrary(g_coreModule);
        g_coreModule = nullptr;
        g_coreDetachFn = nullptr;
        return false;
    }

    ProxyLog::Info("Core_Attach() succeeded -- mod loader is running, "
                   "further output goes to ModLoader\\Logs\\modloader.log");
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        // The game's Binaries\Win64 directory -- everything below is relative
        // to it. Resolved first because ProxyLog needs it, and ProxyLog has to
        // be up before anything that can fail.
        wchar_t gameDir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, gameDir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(gameDir, L'\\'))
            *slash = L'\0';

        ProxyLog::Initialize(gameDir);
        ProxyLog::Info("StarRupture Mod Loader (dwmapi.dll proxy) loaded");
        ProxyLog::LogEnvironment(gameDir);

        // Register ModLoader\ as an extra DLL search directory so that
        // LoadLibraryW("ModLoader\\StarRupture-ModLoader-Core.dll") below (and
        // Core's own delay-loaded StarRupture-ImGui.dll) can find their
        // dependencies there.
        {
            wchar_t modloaderPath[MAX_PATH]{};
            swprintf_s(modloaderPath, L"%s\\ModLoader", gameDir);
            if (SetDllDirectoryW(modloaderPath))
                ProxyLog::Debug("SetDllDirectoryW(%ls) ok", modloaderPath);
            else
                ProxyLog::Warn("SetDllDirectoryW(%ls) failed (error %lu) -- "
                               "Core's dependencies may not resolve",
                               modloaderPath, GetLastError());
        }

        ProxyLog::Info("Initializing dwmapi.dll proxy...");
        if (!DwmapiProxy::Initialize())
        {
            ProxyLog::Error("FATAL: Failed to initialize dwmapi proxy -- DLL load aborted");
            ProxyLog::Shutdown();
            return FALSE;
        }
        ProxyLog::Info("Dwmapi proxy initialized successfully");

#if defined(MODLOADER_CLIENT_BUILD)
        // Self-update (client only).  The update check runs in a separate
        // process (network APIs are unsafe under the loader lock we hold
        // here, but waiting on a child process is fine) and must finish
        // BEFORE the Core DLL is loaded so an accepted update replaces the
        // files on disk and THIS session already runs the new version.
        AutoUpdateLog::Initialize(gameDir, true /* rotate previous log */);
        ProxyLog::Info("Running auto-updater (details in ModLoader\\Logs\\AutoUpdate.log)...");
        UpdaterLaunch::RunUpdaterAndWait();
        ProxyLog::Info("Auto-updater finished");
#endif

        ProxyLog::Info("Loading StarRupture-ModLoader-Core.dll...");
        if (!LoadAndAttachCore())
        {
            DwmapiProxy::Shutdown();
            ProxyLog::Error("Proxy bootstrap failed -- the mod loader is NOT running");
            ProxyLog::Shutdown();
            return FALSE;
        }
    }
    break;

    case DLL_PROCESS_DETACH:
    {
        ProxyLog::Info("DLL_PROCESS_DETACH (process terminating: %s)",
                       lpReserved != nullptr ? "yes" : "no");

        if (g_coreDetachFn)
            g_coreDetachFn(lpReserved != nullptr);

        if (lpReserved != nullptr)
        {
            // Process terminating -- skip the rest to avoid loader-lock issues.
            // Close the log first: nothing else will write to it, and leaving
            // the handle open across process teardown buys nothing.
            ProxyLog::Shutdown();
            break;
        }

        if (g_coreModule)
        {
            FreeLibrary(g_coreModule);
            g_coreModule = nullptr;
        }

        DwmapiProxy::Shutdown();

#if defined(MODLOADER_CLIENT_BUILD)
        AutoUpdateLog::Shutdown();
#endif

        ProxyLog::Info("Proxy shutdown complete");
        ProxyLog::Shutdown();
    }
    break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
