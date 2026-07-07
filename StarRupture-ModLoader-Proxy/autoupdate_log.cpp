// autoupdate_log.cpp -- see autoupdate_log.h for overview.

#include "autoupdate_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <algorithm>

namespace
{
    HANDLE           g_logFile = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION g_lock;
    bool             g_lockInit = false;

    constexpr int kKeepLogs = 10; // AutoUpdate.log + 9 timestamped archives

    // Builds "<game dir>\<suffix>" into outPath.
    void BuildGamePath(wchar_t* outPath, DWORD maxLen, const wchar_t* suffix)
    {
        GetModuleFileNameW(nullptr, outPath, maxLen);
        wchar_t* slash = wcsrchr(outPath, L'\\');
        if (slash)
            wcscpy_s(slash + 1, maxLen - static_cast<rsize_t>(slash + 1 - outPath), suffix);
    }

    // Rotation scheme: AutoUpdate.log is always the log of the CURRENT run.
    // On startup the previous AutoUpdate.log is renamed to
    // AutoUpdate-YYYY-MM-DD_HH-mm-ss.log (timestamp = its last write time),
    // then the oldest archived logs are deleted so that at most kKeepLogs
    // files remain in total (current + kKeepLogs-1 archives).
    void RotateLogs(const wchar_t* logsDir)
    {
        wchar_t current[MAX_PATH]{};
        swprintf_s(current, L"%s\\AutoUpdate.log", logsDir);

        // Archive the previous run's log under its last-write timestamp.
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(current, GetFileExInfoStandard, &fad))
        {
            SYSTEMTIME utc{}, local{};
            FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

            wchar_t archived[MAX_PATH]{};
            swprintf_s(archived, L"%s\\AutoUpdate-%04u-%02u-%02u_%02u-%02u-%02u.log",
                       logsDir, local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond);
            MoveFileExW(current, archived, MOVEFILE_REPLACE_EXISTING);
        }

        // Push/pop: collect all archived logs and delete the oldest until
        // only kKeepLogs-1 archives remain.  The timestamped names sort
        // chronologically, so a simple lexicographic sort is enough.
        wchar_t pattern[MAX_PATH]{};
        swprintf_s(pattern, L"%s\\AutoUpdate-*.log", logsDir);

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

        std::sort(archives.begin(), archives.end()); // oldest first

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

        // Mirror to the debugger regardless of file state
        OutputDebugStringA(line);

        if (!g_lockInit)
            return;

        EnterCriticalSection(&g_lock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(g_logFile, line, static_cast<DWORD>(n), &written, nullptr);
            FlushFileBuffers(g_logFile); // updates are rare; favor durability
        }
        LeaveCriticalSection(&g_lock);
    }
}

namespace AutoUpdateLog
{
    bool Initialize()
    {
        if (!g_lockInit)
        {
            InitializeCriticalSection(&g_lock);
            g_lockInit = true;
        }

        if (g_logFile != INVALID_HANDLE_VALUE)
            return true; // already initialized

        // Ensure ModLoader\ and ModLoader\Logs\ exist before opening the file.
        wchar_t modloaderDir[MAX_PATH]{};
        BuildGamePath(modloaderDir, MAX_PATH, L"ModLoader");
        CreateDirectoryW(modloaderDir, nullptr);

        wchar_t logsDir[MAX_PATH]{};
        BuildGamePath(logsDir, MAX_PATH, L"ModLoader\\Logs");
        CreateDirectoryW(logsDir, nullptr);

        RotateLogs(logsDir);

        wchar_t logPath[MAX_PATH]{};
        swprintf_s(logPath, L"%s\\AutoUpdate.log", logsDir);

        g_logFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_logFile == INVALID_HANDLE_VALUE)
        {
            OutputDebugStringA("[dwmapi-proxy] [WARN] AutoUpdateLog: could not open AutoUpdate.log\n");
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
