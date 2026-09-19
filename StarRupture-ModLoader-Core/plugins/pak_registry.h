#pragma once

struct IPluginPak;

// ---------------------------------------------------------------------------
// PakRegistry -- who mounted what, and the plugin-facing front of it (v67)
//
// hooks/game/pak_mount/ owns the engine side: three resolved entry points
// and the FPakPlatformFile they act on. This file owns everything above
// that line:
//
//  - The registry. One record per pak the loader mounted (or adopted), with
//    its owner. This is what makes a second mount of the same path a no-op
//    that returns the existing handle instead of a duplicate: the engine
//    itself would mount the same file twice without complaint and serve
//    whichever copy sorted first, and there is no way to un-load what was
//    loaded from the wrong one.
//
//  - The cache for paks that do not start life on disk. The engine reads
//    paks through its own file layer and nothing else, so a pak embedded in
//    a plugin DLL (as an RCDATA resource) or built in memory is written to
//    ModLoader\PakCache\<plugin>\ and mounted from there. A hash sidecar
//    skips the write when the cached copy already matches, and a mounted
//    file is never overwritten -- the engine holds it open.
//
//  - The ModActor convention. A mount may name a Blueprint class inside the
//    pak; the loader spawns it in every world that begins play while the pak
//    is mounted, calling PreBeginPlay before and PostBeginPlay after the
//    actor's BeginPlay when the class defines them. That is exactly what
//    UE4SS's BPModLoader does with Content/Paks/LogicMods, so a Blueprint mod
//    written for it runs here unchanged.
//
//  - The game-thread rule. Mounting broadcasts OnPakFileMounted2, whose
//    handlers the engine only runs on the game thread, and asset loading and
//    spawning are game-thread only outright. Every public entry point here
//    runs its work on the game thread: inline when the caller is already
//    there (or the engine has not ticked yet, which is PluginInit with the
//    main thread parked), queued and waited on otherwise.
//
// What plugin unload does NOT do is unmount. The pak stays, the record is
// marked orphaned, and any callback pointer into the DLL is dropped
// (ForgetPlugin, called by PluginManager before FreeLibrary next to the other
// Forget* calls). A later mount of the same path -- the same plugin reloaded,
// typically -- adopts the record. Unmounting under objects still loaded from
// the pak is undefined behaviour in the engine, and the loader cannot tell
// which live objects came from which container, so it never decides that on
// a plugin's behalf.
// ---------------------------------------------------------------------------

namespace PakRegistry
{
    // The interface handed to plugins as hooks->Pak. Never null.
    IPluginPak* GetInterface();

    // Mark every mount this plugin owns as orphaned and drop the callback
    // pointers it registered. Does not unmount anything. Called by
    // PluginManager before FreeLibrary.
    void ForgetPlugin(const char* pluginName);
}
