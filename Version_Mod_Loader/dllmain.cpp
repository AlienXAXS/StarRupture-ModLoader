// dllmain.cpp : DLL entry point. All startup logic lives in core/.
#include "dwmapi_proxy.h"
#include "logging/log.h"
#include "utils/thread_utils.h"
#include "core/globals.h"
#include "core/engine_sync.h"
#include "core/init_thread.h"
#include "core/shutdown.h"

#include "DbgHelp.h"
#pragma comment(lib, "DbgHelp.lib")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        SymRefreshModuleList(GetCurrentProcess());

        LogToFile::Initialize();
        LogToFile::Info("======================================================");
        LogToFile::Info("  StarRupture Mod Loader (dwmapi.dll proxy) loaded");
        LogToFile::Info("======================================================");

        LogToFile::Info("Initializing dwmapi.dll proxy...");
        if (!DwmapiProxy::Initialize())
        {
            LogToFile::Error("FATAL: Failed to initialize dwmapi proxy -- DLL load aborted");
            LogToFile::Shutdown();
            return FALSE;
        }
        LogToFile::Info("Dwmapi proxy initialized successfully");

        g_engineReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_engineReadyEvent)
        {
            LogToFile::Error("FATAL: Failed to create engine-ready event (%lu)", GetLastError());
            DwmapiProxy::Shutdown();
            LogToFile::Shutdown();
            return FALSE;
        }

        g_pluginsLoadedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_pluginsLoadedEvent)
        {
            LogToFile::Error("FATAL: Failed to create init-done event (%lu)", GetLastError());
            CloseHandle(g_engineReadyEvent); g_engineReadyEvent = NULL;
            DwmapiProxy::Shutdown();
            LogToFile::Shutdown();
            return FALSE;
        }

        g_ue4ssReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_ue4ssReadyEvent)
        {
            LogToFile::Warn("Failed to create UE4SS-ready event (%lu) -- UE4SS load will use timeout fallback", GetLastError());
        }

        if (GetCurrentThreadId() == get_main_thread_id())
        {
            LogToFile::Info("DllMain on main thread -- deferring init via QueueUserAPC");
            QueueUserAPC((PAPCFUNC)MainInitApcProc, GetCurrentThread(), (ULONG_PTR)hModule);
        }
        else
        {
            g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
            if (!g_mainInitThread)
            {
                LogToFile::Error("FATAL: Failed to create main init thread (%lu)", GetLastError());
                CloseHandle(g_pluginsLoadedEvent); g_pluginsLoadedEvent = NULL;
                CloseHandle(g_engineReadyEvent);   g_engineReadyEvent   = NULL;
                if (g_ue4ssReadyEvent) { CloseHandle(g_ue4ssReadyEvent); g_ue4ssReadyEvent = NULL; }
                DwmapiProxy::Shutdown();
                LogToFile::Shutdown();
                return FALSE;
            }
        }
    }
    break;

    case DLL_PROCESS_DETACH:
    {
        if (lpReserved != nullptr)
        {
            LogToFile::Info("Process terminating - skipping shutdown to avoid loader-lock / allocator corruption");
            LogToFile::Shutdown();
            break;
        }

        ShutdownAll();
    }
    break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
