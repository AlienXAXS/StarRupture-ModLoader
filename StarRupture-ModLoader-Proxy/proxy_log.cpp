// proxy_log.cpp -- see proxy_log.h for overview.

#include "proxy_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
    HANDLE           g_logFile  = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION g_lock;
    bool             g_lockInit = false;

    constexpr int kKeepLogs = 10; // Proxy.log + 9 timestamped archives

#if defined(MODLOADER_BUILD_TAG)
    constexpr const char* kBuildTag = MODLOADER_BUILD_TAG;
#else
    constexpr const char* kBuildTag = "dev";
#endif

#if defined(MODLOADER_CLIENT_BUILD)
    constexpr const char* kBuildTarget = "client";
#elif defined(MODLOADER_SERVER_BUILD)
    constexpr const char* kBuildTarget = "server";
#else
    constexpr const char* kBuildTarget = "generic";
#endif

    // Same rotation scheme as AutoUpdate.log: Proxy.log is always the CURRENT
    // run, the previous one is renamed to Proxy-YYYY-MM-DD_HH-mm-ss.log
    // (timestamp = its last write time), then the oldest archives are deleted
    // so that at most kKeepLogs files remain (current + kKeepLogs-1 archives).
    void RotateLogs(const wchar_t* logsDir)
    {
        wchar_t current[MAX_PATH]{};
        swprintf_s(current, L"%s\\Proxy.log", logsDir);

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(current, GetFileExInfoStandard, &fad))
        {
            SYSTEMTIME utc{}, local{};
            FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

            wchar_t archived[MAX_PATH]{};
            swprintf_s(archived, L"%s\\Proxy-%04u-%02u-%02u_%02u-%02u-%02u.log",
                       logsDir, local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond);
            MoveFileExW(current, archived, MOVEFILE_REPLACE_EXISTING);
        }

        wchar_t pattern[MAX_PATH]{};
        swprintf_s(pattern, L"%s\\Proxy-*.log", logsDir);

        std::vector<std::wstring> archives;
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    archives.push_back(fd.cFileName);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        std::sort(archives.begin(), archives.end()); // timestamped names sort chronologically

        while (archives.size() > static_cast<size_t>(kKeepLogs - 1))
        {
            wchar_t victim[MAX_PATH]{};
            swprintf_s(victim, L"%s\\%s", logsDir, archives.front().c_str());
            DeleteFileW(victim);
            archives.erase(archives.begin());
        }
    }

    void WriteLine(const char* level, const char* fmt, va_list args)
    {
        char msg[2048];
        vsnprintf(msg, sizeof(msg), fmt, args);

        SYSTEMTIME st{};
        GetLocalTime(&st);

        char line[2200];
        int n = snprintf(line, sizeof(line),
                         "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%-5s] %s\r\n",
                         st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                         level, msg);
        if (n < 0)
            return;
        if (n >= static_cast<int>(sizeof(line)))
            n = static_cast<int>(sizeof(line)) - 1;

        // Mirror to the debugger regardless of file state -- DebugView was the
        // only sink this logger had before Proxy.log existed.
        OutputDebugStringA(line);

        if (!g_lockInit)
            return;

        EnterCriticalSection(&g_lock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(g_logFile, line, static_cast<DWORD>(n), &written, nullptr);
            // Flush every line: the whole point of this log is the boot that
            // ends in a crash or a hang, and buffered output does not survive
            // either of those.
            FlushFileBuffers(g_logFile);
        }
        LeaveCriticalSection(&g_lock);
    }

    // ---- LogEnvironment helpers -------------------------------------------

    void LogFileFacts(const wchar_t* label, const wchar_t* path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        {
            ProxyLog::Error("  %ls: MISSING (%ls, error %lu)", label, path, GetLastError());
            return;
        }

        ULARGE_INTEGER size{};
        size.LowPart  = fad.nFileSizeLow;
        size.HighPart = fad.nFileSizeHigh;

        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

        ProxyLog::Info("  %ls: present, %llu bytes, modified %04u-%02u-%02u %02u:%02u:%02u",
                       label, size.QuadPart,
                       local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond);
    }

    void LogDirectoryListing(const wchar_t* label, const wchar_t* dir)
    {
        wchar_t pattern[MAX_PATH]{};
        swprintf_s(pattern, L"%s\\*", dir);

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            ProxyLog::Warn("  %ls (%ls): cannot enumerate (error %lu)", label, dir, GetLastError());
            return;
        }

        ProxyLog::Info("  %ls (%ls):", label, dir);
        int count = 0;
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                ProxyLog::Info("    [dir]  %ls", fd.cFileName);
            }
            else
            {
                ULARGE_INTEGER size{};
                size.LowPart  = fd.nFileSizeLow;
                size.HighPart = fd.nFileSizeHigh;
                ProxyLog::Info("    [file] %ls (%llu bytes)", fd.cFileName, size.QuadPart);
            }
            ++count;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);

        if (count == 0)
            ProxyLog::Warn("    <empty>");
    }
}

namespace ProxyLog
{
    bool Initialize(const wchar_t* gameDir)
    {
        if (!g_lockInit)
        {
            InitializeCriticalSection(&g_lock);
            g_lockInit = true;
        }

        if (g_logFile != INVALID_HANDLE_VALUE)
            return true; // already initialized

        wchar_t modloaderDir[MAX_PATH]{};
        swprintf_s(modloaderDir, L"%s\\ModLoader", gameDir);
        CreateDirectoryW(modloaderDir, nullptr);

        wchar_t logsDir[MAX_PATH]{};
        swprintf_s(logsDir, L"%s\\ModLoader\\Logs", gameDir);
        CreateDirectoryW(logsDir, nullptr);

        RotateLogs(logsDir);

        wchar_t logPath[MAX_PATH]{};
        swprintf_s(logPath, L"%s\\Proxy.log", logsDir);

        // Append mode, OPEN_ALWAYS: rotation already moved the previous run
        // aside, so this is normally a fresh file. Appending rather than
        // truncating means that if rotation could not rename (the old log left
        // open by an editor, say) this run is added to it instead of erasing
        // it. Shared for read+write+delete so the user can open the log, or zip
        // it up for a bug report, while the game is still running.
        g_logFile = CreateFileW(logPath, FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_logFile == INVALID_HANDLE_VALUE)
        {
            OutputDebugStringA("[dwmapi-proxy] [WARN] could not open ModLoader\\Logs\\Proxy.log\n");
            return false;
        }
        return true;
    }

    void Shutdown()
    {
        if (g_lockInit)
            EnterCriticalSection(&g_lock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_logFile);
            g_logFile = INVALID_HANDLE_VALUE;
        }
        if (g_lockInit)
            LeaveCriticalSection(&g_lock);
    }

    void LogEnvironment(const wchar_t* gameDir)
    {
        Info("---- environment ----");
        Info("  build tag    : %s (%s)", kBuildTag, kBuildTarget);
        Info("  compiled     : %s %s", __DATE__, __TIME__);

        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        Info("  host process : %ls (pid %lu)", exePath, GetCurrentProcessId());
        Info("  command line : %ls", GetCommandLineW());
        Info("  game dir     : %ls", gameDir);

        // Which dwmapi.dll are we? If this path is System32 the game loaded the
        // real one and this code is not running at all; if it is anywhere other
        // than the game dir, the user put the proxy in the wrong folder and
        // something else pulled it in.
        HMODULE selfModule = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&LogEnvironment), &selfModule) && selfModule)
        {
            wchar_t selfPath[MAX_PATH]{};
            GetModuleFileNameW(selfModule, selfPath, MAX_PATH);
            Info("  proxy module : %ls (base 0x%llX)", selfPath,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(selfModule)));
        }
        else
        {
            Warn("  proxy module : could not resolve own module handle (error %lu)", GetLastError());
        }

        wchar_t cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        Info("  current dir  : %ls", cwd);

        // Expected layout next to the game exe.
        wchar_t modloaderDir[MAX_PATH]{};
        swprintf_s(modloaderDir, L"%s\\ModLoader", gameDir);

        wchar_t corePath[MAX_PATH]{};
        swprintf_s(corePath, L"%s\\StarRupture-ModLoader-Core.dll", modloaderDir);
        LogFileFacts(L"Core DLL", corePath);

#if defined(MODLOADER_CLIENT_BUILD)
        wchar_t imguiPath[MAX_PATH]{};
        swprintf_s(imguiPath, L"%s\\StarRupture-ImGui.dll", modloaderDir);
        LogFileFacts(L"ImGui DLL", imguiPath);
#endif

        LogDirectoryListing(L"ModLoader folder", modloaderDir);

        wchar_t pluginsDir[MAX_PATH]{};
        swprintf_s(pluginsDir, L"%s\\Plugins", modloaderDir);
        if (GetFileAttributesW(pluginsDir) != INVALID_FILE_ATTRIBUTES)
            LogDirectoryListing(L"Plugins folder", pluginsDir);
        else
            Info("  Plugins folder (%ls): not present", pluginsDir);

        Info("---------------------");
    }

    void Debug(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        WriteLine("DEBUG", fmt, args);
        va_end(args);
    }

    void Info(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        WriteLine("INFO", fmt, args);
        va_end(args);
    }

    void Warn(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        WriteLine("WARN", fmt, args);
        va_end(args);
    }

    void Error(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        WriteLine("ERROR", fmt, args);
        va_end(args);
    }
}
