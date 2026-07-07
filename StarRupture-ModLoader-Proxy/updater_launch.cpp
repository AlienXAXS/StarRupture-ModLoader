// updater_launch.cpp -- see updater_launch.h for overview.

#include "updater_launch.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "autoupdate_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
    // Fills outPath with "<game exe dir>\<suffix>".
    void BuildGamePath(wchar_t* outPath, DWORD maxLen, const wchar_t* suffix)
    {
        GetModuleFileNameW(nullptr, outPath, maxLen);
        wchar_t* slash = wcsrchr(outPath, L'\\');
        if (slash)
            wcscpy_s(slash + 1, maxLen - static_cast<rsize_t>(slash + 1 - outPath), suffix);
    }

    // Deletes *.autoupdate.bak in a single directory (non-recursive).
    // These are backups of files that were in use during the previous
    // update (old dwmapi.dll, old updater exe) and could not be deleted
    // back then; by the next boot nothing maps them anymore.
    void SweepBackups(const wchar_t* dir)
    {
        wchar_t pattern[MAX_PATH]{};
        swprintf_s(pattern, L"%s\\*.autoupdate.bak", dir);

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return;

        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%s\\%s", dir, fd.cFileName);
            if (DeleteFileW(path))
                AutoUpdateLog::Info("Proxy: removed leftover update backup '%ls'", fd.cFileName);
            else
                AutoUpdateLog::Warn("Proxy: could not remove leftover backup '%ls' (error %lu)",
                                    fd.cFileName, GetLastError());
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
    }
}

namespace UpdaterLaunch
{
    void RunUpdaterAndWait()
    {
        // Clean up backups of files that were in use during the previous
        // update (old dwmapi.dll, old updater exe).  The release ZIP only
        // places files in the game dir root and ModLoader\, so sweeping
        // those two directories covers everything.
        {
            wchar_t dir[MAX_PATH]{};
            BuildGamePath(dir, MAX_PATH, L"");
            size_t len = wcslen(dir);
            if (len > 0 && dir[len - 1] == L'\\')
                dir[len - 1] = L'\0';
            SweepBackups(dir);

            BuildGamePath(dir, MAX_PATH, L"ModLoader");
            SweepBackups(dir);
        }

        wchar_t exePath[MAX_PATH]{};
        BuildGamePath(exePath, MAX_PATH, L"ModLoader\\StarRupture-ModLoader-Updater.exe");

        if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES)
        {
            AutoUpdateLog::Warn("Proxy: updater exe not found (%ls) -- update check skipped", exePath);
            return;
        }

        wchar_t gameDir[MAX_PATH]{};
        BuildGamePath(gameDir, MAX_PATH, L"");
        // Strip the trailing backslash left by the empty suffix
        size_t len = wcslen(gameDir);
        if (len > 0 && gameDir[len - 1] == L'\\')
            gameDir[len - 1] = L'\0';

        // CreateProcess may modify the command line buffer -> must be writable.
        wchar_t cmdLine[MAX_PATH * 2]{};
        swprintf_s(cmdLine, L"\"%s\" \"%s\"", exePath, gameDir);

        AutoUpdateLog::Info("Proxy: launching updater and waiting for it to finish");

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi))
        {
            AutoUpdateLog::Error("Proxy: could not launch updater (error %lu) -- update check skipped",
                                 GetLastError());
            return;
        }

        // Waiting on a child process in DllMain is safe (the child has its
        // own loader and never touches this process).  The wait is unbounded
        // because the updater may be sitting on a user prompt; every
        // non-interactive step inside it has its own network timeout.
        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode = static_cast<DWORD>(-1);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        switch (exitCode)
        {
        case 0: AutoUpdateLog::Info("Proxy: updater finished -- no update installed"); break;
        case 1: AutoUpdateLog::Info("Proxy: updater finished -- update installed, loading new version"); break;
        case 2: AutoUpdateLog::Info("Proxy: updater finished -- update declined by user"); break;
        case 3: AutoUpdateLog::Warn("Proxy: updater finished -- update failed and was rolled back"); break;
        default:
            AutoUpdateLog::Warn("Proxy: updater exited abnormally (code %lu) -- continuing with current install",
                                exitCode);
            break;
        }
    }
}

#else // !MODLOADER_CLIENT_BUILD

namespace UpdaterLaunch
{
    void RunUpdaterAndWait() {}
}

#endif
