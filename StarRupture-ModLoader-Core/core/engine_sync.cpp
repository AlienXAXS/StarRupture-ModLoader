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
    LogToFile::Info("[init] Main-thread APC fired -- starting Stage 1 with the game held");

    g_mainInitThread = CreateThread(nullptr, 0, MainInitThreadProc, nullptr, 0, nullptr);
    if (!g_mainInitThread)
    {
        LogToFile::Error("FATAL: Failed to create main init thread from APC (%lu)", GetLastError());
        g_mainThreadParked = false;
        if (g_stage1DoneEvent) { CloseHandle(g_stage1DoneEvent); g_stage1DoneEvent = NULL; }
        if (g_pluginsLoadedEvent)
            SetEvent(g_pluginsLoadedEvent);
        return;
    }

    LogToFile::Info("[init] Init thread started (tid %lu)", GetThreadId(g_mainInitThread));

    if (!g_mainThreadParked || !g_stage1DoneEvent)
    {
        LogToFile::Info("[init] No Stage 1 gate -- main thread continues booting alongside Stage 1");
        return;
    }

    // Hold the game main thread here for the whole of Stage 1 rather than
    // letting the init thread SuspendThread() it.
    //
    // This APC runs from NtTestAlert at the end of process init: the loader
    // lock and the heap locks are all free at this exact point, so stopping
    // here cannot deadlock anything Stage 1 does. SuspendThread() has no such
    // guarantee -- it freezes the main thread at whatever instruction it has
    // reached, and once the game's entry point starts running that is very
    // often inside LdrLoadDll or RtlAllocateHeap. Stage 1 then creates the
    // splash window (loader lock, first-time theme/IME DLL loads) and runs the
    // pattern preflight (heap), blocks on the lock the frozen thread owns, and
    // the game hangs with nothing further written to the log. It is a race, so
    // it only ever bit some machines (v1.16.0).
    constexpr DWORD kStage1TimeoutMs = 180'000;
    LogToFile::Info("[init] Main thread parked at the Stage 1 gate (timeout %lu ms)", kStage1TimeoutMs);

    const ULONGLONG gateStart = GetTickCount64();
    const DWORD r = WaitForSingleObject(g_stage1DoneEvent, kStage1TimeoutMs);
    const ULONGLONG gateMs = GetTickCount64() - gateStart;

    if (r == WAIT_OBJECT_0)
        LogToFile::Info("[init] Stage 1 gate released after %llu ms -- main thread resuming game boot", gateMs);
    else
        LogToFile::Error("[init] Stage 1 gate returned %lu after %llu ms -- releasing the main thread anyway", r, gateMs);

    // The handle is deliberately left open and closed by ShutdownAll: on the
    // timeout path the init thread is still alive and will SetEvent on it.
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
