// main.cpp -- entry point for StarRupture-ModLoader-Updater.exe.
//
// Invoked by the dwmapi.dll proxy from the game's DllMain (which waits for
// this process to exit before loading the Core DLL).  argv[1] is the game's
// Binaries\Win64 directory; when absent (manual run) it is derived from the
// exe's own location (exe lives in <game>\ModLoader\).

#include "updater.h"
#include "autoupdate_log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t gameDir[MAX_PATH]{};

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2)
    {
        wcscpy_s(gameDir, argv[1]);
    }
    else
    {
        // Manual run: exe is at <game>\ModLoader\StarRupture-ModLoader-Updater.exe
        GetModuleFileNameW(nullptr, gameDir, MAX_PATH);
        wchar_t* slash = wcsrchr(gameDir, L'\\');
        if (slash) *slash = L'\0'; // strip exe name -> ModLoader dir
        slash = wcsrchr(gameDir, L'\\');
        if (slash) *slash = L'\0'; // strip ModLoader -> game dir
    }
    if (argv)
        LocalFree(argv);

    if (gameDir[0] == L'\0')
        return UPDATER_EXIT_UP_TO_DATE;

    // The proxy already rotated and created AutoUpdate.log for this boot;
    // append to it (falls back to OutputDebugString if unavailable).
    AutoUpdateLog::Initialize(gameDir, false /* no rotation, append */);

    int result = RunUpdater(gameDir);

    AutoUpdateLog::Info("Updater: exiting with code %d", result);
    AutoUpdateLog::Shutdown();
    return result;
}
