#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Runtime pak mounting -- the engine side
//
// This module owns the three AOB-resolved engine entry points that let the
// loader mount and unmount pak files while the game is running, and the
// FPakPlatformFile instance they operate on. Everything above it (who owns a
// mount, the cache for paks embedded in a DLL, the plugin interface, the
// console command) lives in plugins/pak_registry.*.
//
// How the engine does it
// ----------------------
// FPakPlatformFile is the IPlatformFile layer that serves reads out of pak
// files, and in a shipping build it is the "PakFile" entry in the platform
// file chain that FPlatformFileManager::Get() owns. The engine's own runtime
// mount path -- FCoreDelegates::MountPak, used by chunk downloaders and game
// feature plugins -- is bound to FPakPlatformFile::HandleMountPakDelegate,
// which takes a path and a priority, calls FPakPlatformFile::Mount, and
// returns the new IPakFile*. Mount also opens the IoStore container (the
// .utoc/.ucas pair) that sits next to the pak when there is one and registers
// it with the package store, which is what makes cooked UE5 packages inside
// it loadable by name. This game ships its content as IoStore containers, so
// a mod pak that carries assets must be an IoStore triplet too; a bare .pak
// only mounts loose (non-package) files.
//
// Unmounting mirrors that: HandleUnmountPakDelegate -> FPakPlatformFile::
// Unmount drops the pak from the list, unmounts its container from the I/O
// dispatcher and the package store, and tells the precacher. What it cannot
// do is un-load anything already loaded out of it. Objects created from a
// pak's packages stay alive with their bulk data now unreachable, so
// unmounting a pak whose assets are in use is the caller's risk, not the
// engine's -- see the plugin-facing notes in pak_registry.h.
//
// Resolution
// ----------
// The two delegate handlers have distinctive bodies and are found directly.
// The FPakPlatformFile* is not a global: it is reached through
// FPlatformFileManager::Get() (eight bytes, `lea rax, Singleton ; ret`, and
// the same eight bytes as every other singleton getter) and
// FindPlatformFile(L"PakFile"). Both are decoded from the two call
// instructions in FPakFileModule::ShutdownModule, which calls them back to
// back -- the one caller in the binary whose bytes around those calls are
// distinctive enough to anchor on. See scan_patterns.h for the instruction
// listing the offsets are taken from.
//
// Threading
// ---------
// Mount and Unmount take the engine's own pak-list lock and the package
// store's, so they are safe from any thread as far as their own state goes.
// The OnPakFileMounted2 broadcast at the end of Mount, however, runs handlers
// (localization, shader library) that the engine only ever runs on the game
// thread, and the loader keeps that convention: pak_registry dispatches every
// mount and unmount to the game thread. During startup, while the main thread
// is parked in the engine-init hook, the init thread is effectively alone in
// the process and calls straight through.
// ---------------------------------------------------------------------------

namespace Hooks::PakMount
{
    // One mounted pak as the engine sees it. Filenames are whatever spelling
    // the engine stored -- the game's own paks are relative
    // ("../../../StarRupture/Content/Paks/x.pak"), ours are absolute.
    struct MountedPak
    {
        std::wstring pakFilename;
        std::wstring mountPoint;
        int          readOrder     = 0;
        int          pakchunkIndex = -1;
        int          numFiles      = 0;   // entries in the pak's own index, not IoStore packages
        void*        pakFile       = nullptr;  // IPakFile*, valid while mounted
    };

    enum class Status
    {
        Ok,
        Unavailable,   // patterns unresolved, or no FPakPlatformFile in the chain
        Refused,       // the engine returned failure (unreadable, bad magic, encrypted, ...)
        Fault          // the engine call raised an exception
    };

    // Scan the three patterns and locate the FPakPlatformFile. Safe to call
    // more than once; scans on the first call only.
    bool Resolve();

    // True once Resolve() found everything, including the platform file.
    bool IsAvailable();

    // The FPakPlatformFile*, or null when unavailable.
    void* GetPakPlatformFile();

    // Mount `pakPath` at `order` (see NormalizePath for the spelling to use).
    // order < 0 lets the engine derive it from the path, which for anything
    // outside Content/Paks means 0 -- lower than every stock pak.
    Status Mount(const std::wstring& pakPath, int order, MountedPak* outInfo);

    // Unmount by the exact filename it was mounted with.
    Status Unmount(const std::wstring& pakPath);

    // Copy every pak the engine currently has mounted. Walks
    // FPakPlatformFile::PakFiles under the engine's own lock. Returns false
    // when unavailable or when the walk faulted (out is then whatever was
    // gathered before the fault).
    bool EnumerateMounted(std::vector<MountedPak>& out);

    // Find one mounted pak by path, comparing normalized spellings (so the
    // engine's relative name for a stock pak matches its absolute one).
    bool FindMounted(const std::wstring& pakPath, MountedPak* out);

    // Absolute path with forward slashes, resolved against the game exe's
    // directory when relative (which is what the engine's "../../../" names
    // are relative to). Case is preserved; compare with _wcsicmp.
    std::wstring NormalizePath(const std::wstring& path);
}
