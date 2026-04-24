#include "startup_utils.h"
#include "globals.h"
#include "../logging/log.h"

#include <windows.h>
#include <Psapi.h>
#include <VersionHelpers.h>

#pragma comment(lib, "psapi.lib")

std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return path;
}

std::wstring GetExeDirPath(const wchar_t* filename)
{
    return GetExeDir() + filename;
}

void LogStartupEnvironment()
{
    LogToFile::Info("Process ID: %lu", GetCurrentProcessId());

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    LogToFile::Info("Executable: %ls", exePath);

    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    LogToFile::Info("Working directory: %ls", cwd);

    LogToFile::Info("Command line: %ls", GetCommandLineW());

    HMODULE mainModule = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), mainModule, &mi, sizeof(mi)))
    {
        LogToFile::Info("Main module base: 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)));
        LogToFile::Info("Main module size: 0x%lX (%lu KB)", static_cast<unsigned long>(mi.SizeOfImage), mi.SizeOfImage / 1024);
        LogToFile::Info("Main module entry: 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mi.EntryPoint)));
    }
    else
    {
        LogToFile::Warn("Could not retrieve main module info");
    }

    if (IsWindows10OrGreater()) LogToFile::Info("OS version: Windows 10 or greater");
    else if (IsWindows8Point1OrGreater()) LogToFile::Info("OS version: Windows 8.1");
    else if (IsWindows8OrGreater())       LogToFile::Info("OS version: Windows 8");
    else if (IsWindows7OrGreater())    LogToFile::Info("OS version: Windows 7");
    else if (IsWindowsVistaOrGreater())   LogToFile::Info("OS version: Windows Vista");
    else   LogToFile::Info("OS version: Windows XP or older");

    LogToFile::Info("OS type: %s", IsWindowsServer() ? "Server" : "Client/Workstation");

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus))
    {
        LogToFile::Info("System RAM: %llu MB total, %llu MB available",
            memStatus.ullTotalPhys / (1024 * 1024),
            memStatus.ullAvailPhys / (1024 * 1024));
    }
}

void LoadUE4SS()
{
    const std::wstring iniPath = GetExeDirPath(L"modloader.ini");

    if (!GetPrivateProfileIntW(L"UE4SS", L"Enabled", 1, iniPath.c_str()))
    {
        LogToFile::Info("UE4SS loading disabled in modloader.ini");
        return;
    }

    wchar_t relPath[MAX_PATH]{};
    GetPrivateProfileStringW(L"UE4SS", L"Path", L"ue4ss\\ue4ss.dll", relPath, MAX_PATH, iniPath.c_str());

    const std::wstring fullPath = GetExeDir() + relPath;

    LogToFile::Info("Loading UE4SS from: %ls", fullPath.c_str());

    HMODULE hUE4SS = LoadLibraryW(fullPath.c_str());
    if (hUE4SS)
    {
        LogToFile::Info("UE4SS loaded successfully (handle 0x%llX)",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hUE4SS)));
    }
    else
    {
        LogToFile::Warn("UE4SS failed to load (error %lu): %ls", GetLastError(), fullPath.c_str());
    }
}
