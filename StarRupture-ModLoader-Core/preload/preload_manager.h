#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PreloadManager -- discovers, vets and runs the DLLs in ModLoader\Preload\
// during Stage 1, while the game main thread is parked and the executable's
// entry point has not yet run.
//
// See preload/preload_interface.h for what a preload plugin is and why the
// window matters. This header is about the three rules the manager itself
// exists to enforce.
//
// 1. IT NEVER STOPS THE GAME BOOTING.
//    Every failure -- a bad version, the wrong build target, a pattern that
//    does not resolve or resolves twice, a crash in any entry point -- unloads
//    that one DLL and moves on. The loader continues, the game starts. This is
//    deliberately the opposite of PatternPreflight, which disables the whole
//    mod loader when one of the loader's OWN patterns breaks: a preload plugin
//    is somebody's mod, and an outdated one must be safe to leave installed
//    across a game update.
//
// 2. IT ONLY RUNS ON THE PARKED PATH.
//    RunPhase() does nothing unless g_mainThreadParked is set. On the injected
//    path the main thread was stopped with SuspendThread at an arbitrary
//    instruction -- routinely inside LdrLoadDll or RtlAllocateHeap -- and
//    LoadLibrary from the init thread would deadlock against the loader lock it
//    is holding. That is the v1.16.0 hang, and it is not worth re-creating for
//    a phase whose entire value ("before the game starts") is already gone by
//    then: on that path the game has been running for a while.
//
// 3. IT LEAVES A BREADCRUMB.
//    The name of the DLL currently being loaded is written to
//    ModLoader\Preload\.state and cleared when it finishes. A file still there
//    on the next launch means that plugin did not survive its turn, so it is
//    skipped and the log says why. Without it, a preload plugin that hard-hangs
//    or hard-crashes leaves a game that will not start and a log that stops
//    mid-sentence, with nothing to act on but trial and error.
//
// Kill switches, for when it still goes wrong:
//   -NoPreload                        command line, skips the phase entirely
//   [Preload] Enabled=0               in ModLoader\modloader.ini
//   [Preload] Disabled=a.dll,b.dll    skip named plugins
//
// The ini is read with the raw profile API rather than the config manager,
// which does not exist yet at Stage 1.
// ---------------------------------------------------------------------------

namespace PreloadManager
{
    // What happened to one DLL in the Preload folder. Kept for the whole
    // session so the console and the client UI can show it long after Stage 1.
    enum class Status
    {
        Running = 0,      // loaded, scanned clean, PreloadInit returned true
        Disabled,         // named in [Preload] Disabled, or the phase was off
        SkippedAfterCrash,// the breadcrumb said it did not survive last launch
        BadInterface,     // interfaceVersion outside [MIN, MAX]
        WrongTarget,      // built for the other executable
        MissingExports,   // no GetPreloadInfo / PreloadInit
        HookScanFailed,   // a pattern missed, matched twice, or was the wrong kind
        InitFailed,       // PreloadInit returned false
        Crashed,          // faulted in one of its entry points
        LoadFailed,       // LoadLibrary failed outright
    };

    struct Record
    {
        std::string name;        // PreloadInfo name, or the file name if unknown
        std::string version;
        std::string author;
        std::string fileName;    // bare DLL file name
        Status      status = Status::LoadFailed;
        std::string detail;      // one line of why, for anything that is not Running
        int         priority = 0;
        int         hooksInstalled = 0;
    };

    // Creates ModLoader\Preload if it is missing. Called from Core_Attach as
    // well as from the phase itself, because the folder has to exist whether or
    // not the phase ever runs: preflight failing, -NoPreload and the injected
    // path all return before discovery, and someone looking for where preload
    // plugins go should find an empty folder rather than nothing at all.
    void EnsureFolderExists();

    // Runs the whole phase. Call from Stage 1 only, with the main thread held.
    // Never throws, never returns an error: the caller has nothing it could
    // usefully do differently.
    void RunPhase();

    // Removes every hook installed by every preload plugin and calls their
    // PreloadShutdown. Called from the loader's shutdown path.
    void Shutdown();

    // Snapshot of every DLL the phase looked at, in the order it ran them.
    std::vector<Record> GetRecords();

    // Short human-readable form of a status, for the console and the UI.
    const char* StatusName(Status status);

    // True when the phase ran at all (as opposed to being switched off or
    // skipped because the main thread was not parked). Lets the console say
    // "preload is off" rather than "no preload plugins found".
    bool DidRun();

    // Why the phase did not run, when DidRun() is false. Empty otherwise.
    std::string GetSkipReason();
}
