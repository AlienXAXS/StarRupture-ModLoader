#include "core_entry.h"
#include "globals.h"
#include "init_thread.h"
#include "engine_sync.h"
#include "shutdown.h"
#include "../logging/log.h"
#include "../utils/thread_utils.h"
#include "DbgHelp.h"
#pragma comment(lib, "DbgHelp.lib")

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

#if defined(MODLOADER_CLIENT_BUILD)
#define MODLOADER_BUILD_KIND "client"
#elif defined(MODLOADER_SERVER_BUILD)
#define MODLOADER_BUILD_KIND "server"
#else
#define MODLOADER_BUILD_KIND "generic"
#endif

static bool g_coreAttached = false;

static void MoveDirectoryContents(const wchar_t* srcDir, const wchar_t* dstDir)
{
    wchar_t pattern[MAX_PATH]{};
    swprintf_s(pattern, L"%s\\*", srcDir);

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        wchar_t src[MAX_PATH]{}, dst[MAX_PATH]{};
        swprintf_s(src, L"%s\\%s", srcDir, fd.cFileName);
        swprintf_s(dst, L"%s\\%s", dstDir, fd.cFileName);

        if (!MoveFileW(src, dst))
            LogToFile::Warn("MigratePlugins: could not move %ls (err %lu) -- skipping", fd.cFileName, GetLastError());
        else
            LogToFile::Info("MigratePlugins: moved %ls", fd.cFileName);
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);
    RemoveDirectoryW(srcDir);
}

static void MigrateLegacyConfigFiles(const wchar_t* exeDir, const wchar_t* modloaderDir)
{
    // Delete the old log file outright -- moving it while logging is active makes no sense
    {
        wchar_t log[MAX_PATH]{};
        swprintf_s(log, L"%s\\modloader.log", exeDir);
        DeleteFileW(log);
    }

    static const wchar_t* kFiles[] = {
        L"modloader.ini", L"modloader_imgui.ini", L"modloader.txt", L"scan_cache.ini",
        L"update_state.ini"
    };
    for (const wchar_t* name : kFiles)
    {
        wchar_t src[MAX_PATH]{};
        swprintf_s(src, L"%s\\%s", exeDir, name);

        if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES)
            continue;

        CreateDirectoryW(modloaderDir, nullptr);

        wchar_t dst[MAX_PATH]{};
        swprintf_s(dst, L"%s\\%s", modloaderDir, name);

        // MOVEFILE_REPLACE_EXISTING: if a default config was already written to
        // ModLoader\ on a previous run, the old user config should win.
        if (!MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING))
        {
            LogToFile::Warn("MigrateFiles: could not move %ls to ModLoader\\ (err %lu) -- deleting", name, GetLastError());
            DeleteFileW(src);
        }
        else
        {
            LogToFile::Info("MigrateFiles: moved %ls -> ModLoader\\%ls", name, name);
        }
    }
}

static void MigrateLegacyPluginsFolder(const wchar_t* exeDir, const wchar_t* modloaderDir)
{
    wchar_t oldPlugins[MAX_PATH]{};
    swprintf_s(oldPlugins, L"%s\\Plugins", exeDir);

    if (GetFileAttributesW(oldPlugins) == INVALID_FILE_ATTRIBUTES)
        return;

    CreateDirectoryW(modloaderDir, nullptr);

    wchar_t newPlugins[MAX_PATH]{};
    swprintf_s(newPlugins, L"%s\\Plugins", modloaderDir);

    if (MoveFileW(oldPlugins, newPlugins))
    {
        LogToFile::Info("MigrateFiles: moved Plugins\\ -> ModLoader\\Plugins\\");
    }
    else
    {
        // Destination already exists -- move contents individually
        LogToFile::Info("MigrateFiles: ModLoader\\Plugins\\ exists, merging contents of old Plugins\\");
        CreateDirectoryW(newPlugins, nullptr);
        MoveDirectoryContents(oldPlugins, newPlugins);
    }
}

static void MigrateOrDeleteLegacyFiles()
{
    wchar_t exeDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (!slash) return;
    *slash = L'\0';

    wchar_t modloaderDir[MAX_PATH]{};
    swprintf_s(modloaderDir, L"%s\\ModLoader", exeDir);

    MigrateLegacyConfigFiles(exeDir, modloaderDir);
    MigrateLegacyPluginsFolder(exeDir, modloaderDir);
}

BOOL Core_Attach()
{
    SymRefreshModuleList(GetCurrentProcess());

    LogToFile::Initialize();
    LogToFile::Info("======================================================");
    LogToFile::Info("  StarRupture Mod Loader Core loaded");
    LogToFile::Info("======================================================");

    // Identity of this build and process, logged here rather than only in
    // LogStartupEnvironment() (Stage 2) so a log that stops during Stage 1
    // still says which modloader and which exe produced it.
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        LogToFile::Info("[init] Build: %s (%s), PID %lu", MODLOADER_BUILD_TAG, MODLOADER_BUILD_KIND, GetCurrentProcessId());
        LogToFile::Info("[init] Executable: %ls", exePath);
    }

    const DWORD attachThreadId = GetCurrentThreadId();
    const DWORD mainThreadId   = get_main_thread_id();
    LogToFile::Info("[init] Core_Attach on thread %lu (main thread is %lu)", attachThreadId, mainThreadId);

    LogToFile::Info("[init] Migrating legacy config/plugin files...");
    MigrateOrDeleteLegacyFiles();
    LogToFile::Info("[init] Legacy file migration done");

#ifdef MODLOADER_CLIENT_BUILD
    {
        wchar_t imguiDllPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, imguiDllPath, MAX_PATH);
        wchar_t* sl = wcsrchr(imguiDllPath, L'\\');
        if (sl)
            wcscpy_s(sl + 1, MAX_PATH - static_cast<rsize_t>(sl + 1 - imguiDllPath),
                L"ModLoader\\StarRupture-ImGui.dll");

        if (GetFileAttributesW(imguiDllPath) != INVALID_FILE_ATTRIBUTES)
        {
            LogToFile::Info("[init] ImGui DLL present: %ls", imguiDllPath);
        }
        else
        {
            LogToFile::Error("FATAL: ModLoader\\StarRupture-ImGui.dll not found -- aborting");
            MessageBoxW(nullptr,
                L"StarRupture-ImGui.dll is missing from the ModLoader folder.\n\n"
                L"Please reinstall the mod loader.",
                L"StarRupture Mod Loader -- Missing File",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return FALSE;
        }
    }
#endif

    g_engineReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_engineReadyEvent)
    {
        LogToFile::Error("FATAL: Failed to create engine-ready event (%lu)", GetLastError());
        return FALSE;
    }

    g_pluginsLoadedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_pluginsLoadedEvent)
    {
        LogToFile::Error("FATAL: Failed to create init-done event (%lu)", GetLastError());
        CloseHandle(g_engineReadyEvent); g_engineReadyEvent = NULL;
        return FALSE;
    }

    g_ue4ssReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_ue4ssReadyEvent)
    {
        LogToFile::Warn("Failed to create UE4SS-ready event (%lu) -- UE4SS load will use timeout fallback", GetLastError());
    }

    g_pluginsReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_pluginsReadyEvent)
    {
        LogToFile::Error("FATAL: Failed to create plugins-ready event (%lu)", GetLastError());
        CloseHandle(g_pluginsLoadedEvent); g_pluginsLoadedEvent = NULL;
        CloseHandle(g_engineReadyEvent);   g_engineReadyEvent   = NULL;
        if (g_ue4ssReadyEvent) { CloseHandle(g_ue4ssReadyEvent); g_ue4ssReadyEvent = NULL; }
        return FALSE;
    }

    LogToFile::Info("[init] Sync events created");

    if (attachThreadId == mainThreadId)
    {
        // Stage 1 must run with the game main thread stopped. The APC gives us
        // a point where the main thread is provably holding no loader or heap
        // lock, so it parks itself on this event instead of the init thread
        // suspending it from the outside -- see MainInitApcProc.
        g_stage1DoneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_stage1DoneEvent)
            LogToFile::Warn("Failed to create Stage 1 gate event (%lu) -- main thread will be suspended instead", GetLastError());
        else
            g_mainThreadParked = true;

        LogToFile::Info("Core_Attach on main thread -- deferring init via QueueUserAPC");
        if (!QueueUserAPC((PAPCFUNC)MainInitApcProc, GetCurrentThread(), 0))
            LogToFile::Error("FATAL: QueueUserAPC failed (%lu) -- the mod loader will not start", GetLastError());
    }
    else
    {
        LogToFile::Info("[init] Core_Attach off the main thread -- starting init thread directly");
        g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
        if (!g_mainInitThread)
        {
            LogToFile::Error("FATAL: Failed to create main init thread (%lu)", GetLastError());
            CloseHandle(g_pluginsReadyEvent);  g_pluginsReadyEvent  = NULL;
            CloseHandle(g_pluginsLoadedEvent); g_pluginsLoadedEvent = NULL;
            CloseHandle(g_engineReadyEvent);   g_engineReadyEvent   = NULL;
            if (g_ue4ssReadyEvent) { CloseHandle(g_ue4ssReadyEvent); g_ue4ssReadyEvent = NULL; }
            return FALSE;
        }
    }

    g_coreAttached = true;
    LogToFile::Info("[init] Core_Attach complete -- returning to the proxy");
    return TRUE;
}

void Core_Detach(BOOL processTerminating)
{
    if (!g_coreAttached)
        return;

    if (processTerminating)
    {
        LogToFile::Info("Process terminating -- skipping shutdown to avoid loader-lock / allocator corruption");
        return;
    }

    ShutdownAll();
    g_coreAttached = false;
}
