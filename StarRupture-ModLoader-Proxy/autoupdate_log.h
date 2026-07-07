#pragma once

// ---------------------------------------------------------------------------
// AutoUpdateLog -- dedicated on-disk log for the mod loader's self-update
// system.  Shared source: compiled into BOTH the dwmapi.dll proxy and
// StarRupture-ModLoader-Updater.exe (each process gets its own instance,
// but they append to the same file -- see below).
//
// Writes to <game>\ModLoader\Logs\AutoUpdate.log (the Logs folder is
// created if missing).  AutoUpdate.log is always the current run; when
// Initialize() is called with rotate=true (the proxy, once per game boot)
// the previous one is archived to
//   AutoUpdate-YYYY-MM-DD_HH-mm-ss.log
// (timestamp = its last write time) and the oldest archives are deleted so
// the last 10 runs are always kept.  The updater exe initializes with
// rotate=false and appends to the log the proxy just created.
//
// The file is opened in append mode (FILE_APPEND_DATA) with full read/write
// sharing, so the proxy and the updater can interleave lines safely.
//
// All functions are safe to call from any thread.  Before Initialize() (or
// if it fails, e.g. read-only game folder) messages fall back to
// OutputDebugStringA only -- logging must never break the loader.
// ---------------------------------------------------------------------------

namespace AutoUpdateLog
{
    // gameDir: the game's Binaries\Win64 directory (no trailing backslash).
    // rotate:  archive the previous AutoUpdate.log first (proxy: true,
    //          updater exe: false -- it appends to the proxy's log).
    // Returns false if the file could not be opened (logging then falls
    // back to OutputDebugStringA only).
    bool Initialize(const wchar_t* gameDir, bool rotate);

    void Shutdown();

    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);
}
