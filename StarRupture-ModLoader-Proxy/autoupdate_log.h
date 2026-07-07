#pragma once

// ---------------------------------------------------------------------------
// AutoUpdateLog -- dedicated on-disk log for the proxy's self-update system.
//
// Writes to <game>\ModLoader\Logs\AutoUpdate.log (the Logs folder is created
// if missing).  AutoUpdate.log is always the current run; on every
// Initialize() the previous one is archived to
//   AutoUpdate-YYYY-MM-DD_HH-mm-ss.log
// (timestamp = its last write time) and the oldest archives are deleted so
// the last 10 runs are always kept.
//
// This logger is intentionally separate from Core's modloader.log (see
// proxy_log.h for why the proxy must not share Core's LogToFile) and from
// ProxyLog (OutputDebugString only).  Every line is also mirrored to
// OutputDebugStringA so DebugView shows the same information.
//
// All functions are safe to call from any thread.  Before Initialize() (or
// if it fails, e.g. read-only game folder) messages fall back to
// OutputDebugStringA only -- logging must never break the loader.
// ---------------------------------------------------------------------------

namespace AutoUpdateLog
{
    // Creates ModLoader\Logs, rotates old logs, opens a fresh AutoUpdate.log.
    // Returns false if the file could not be opened (logging then falls back
    // to OutputDebugStringA only).
    bool Initialize();

    void Shutdown();

    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);
}
