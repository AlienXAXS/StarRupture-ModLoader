#include "engine_sync.h"
#include "globals.h"
#include "startup_utils.h"
#include "../logging/log.h"
#include "../logging/ue_log.h"
#include "../memory_scanner/scanner.h"
#include "../UI/splash_window.h"
#include "../hooks/game/gobject_walk/gobject_walk.h"

#include <thread>

// Forward declaration -- defined in core/init_thread.cpp
DWORD WINAPI MainInitThreadProc(LPVOID);

VOID CALLBACK MainInitApcProc(ULONG_PTR)
{
    g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
    if (!g_mainInitThread)
    {
        LogToFile::Error("FATAL: Failed to create main init thread from APC (%lu)", GetLastError());
        if (g_pluginsLoadedEvent)
            SetEvent(g_pluginsLoadedEvent);
    }
}

void OnEngineInitForUELog()
{
    if (UELog::Initialize(+[](const std::string& pattern) -> uintptr_t {
        return Scanner::FindPatternInMainModule(std::string("BASIC_LOGV"), pattern);
        }))
    {
        LogToFile::Info("[ModLoader] UE log bridge active - messages will also appear in StarRupture.log");
    }

    std::thread([]()
    {
        if (g_ue4ssReadyEvent)
        {
            constexpr DWORD kTimeoutMs = 15'000;
            DWORD r = WaitForSingleObject(g_ue4ssReadyEvent, kTimeoutMs);
            if (r == WAIT_TIMEOUT)
                LogToFile::Warn("UE4SS load: timed out waiting for detour to unwind (%lu ms) -- loading anyway", kTimeoutMs);
            else if (r != WAIT_OBJECT_0)
                LogToFile::Warn("UE4SS load: WaitForSingleObject returned unexpected value %lu -- loading anyway", r);
        }
        LoadUE4SS();
    }).detach();
}

// Dev-only: dump every discovered UClass name (and its directly-declared
// UFunction names) to a flat text file, for manual curation into a generated
// header of compile-time name constants used for plugin IDE autocomplete.
// Opt-in via modloader.ini [Debug] ExportKnownGObjects=1 (default 0) so this
// never runs unexpectedly on a normal launch.
void OnEngineInitForGObjectExport()
{
    const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");
    if (!GetPrivateProfileIntW(L"Debug", L"ExportKnownGObjects", 0, iniPath.c_str()))
        return;

    const std::wstring outPath = GetModLoaderDirPath(L"known_gobjects_dump.txt");
    std::string outPathUtf8;
    outPathUtf8.reserve(outPath.size());
    for (wchar_t c : outPath)
        outPathUtf8.push_back(static_cast<char>(c));

    LogToFile::Info("[ModLoader] ExportKnownGObjects enabled -- dumping known classes/functions to %ls", outPath.c_str());
    int written = Hooks::GObjectWalk::ExportKnownClassesAndFunctions(outPathUtf8.c_str());
    LogToFile::Info("[ModLoader] GObjects export wrote %d entries", written);
}

void WaitForEngineReady()
{
    if (!g_engineReadyEvent)
        return;

    Splash::SetStatus(L"Waiting for engine...");
    Splash::SetProgress(0.45f);
    LogToFile::Info("[ModLoader] Waiting for engine initialization...");

    static constexpr DWORD kEngineWaitTimeoutMs = 120'000;
    HANDLE waitHandles[] = { g_engineReadyEvent };
    for (;;)
    {
        DWORD r = MsgWaitForMultipleObjects(1, waitHandles, FALSE, kEngineWaitTimeoutMs, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
        {
            LogToFile::Info("[ModLoader] Engine ready -- proceeding to load plugins");
            break;
        }
        if (r == WAIT_OBJECT_0 + 1)
        {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            continue;
        }
        LogToFile::Warn("[ModLoader] Timed out waiting for engine init (%lu ms) -- loading plugins anyway", kEngineWaitTimeoutMs);
        break;
    }

    CloseHandle(g_engineReadyEvent);
    g_engineReadyEvent = NULL;
}
