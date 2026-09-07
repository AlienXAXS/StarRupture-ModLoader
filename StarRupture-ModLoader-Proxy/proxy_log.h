#pragma once

// ---------------------------------------------------------------------------
// ProxyLog -- on-disk log for the dwmapi.dll proxy shim.
//
// Writes to <game>\ModLoader\Logs\Proxy.log, using the same rotation scheme
// as AutoUpdate.log: Proxy.log is always the current run, the previous one is
// archived to Proxy-YYYY-MM-DD_HH-mm-ss.log (timestamp = its last write time)
// and the oldest archives are deleted so the last 10 runs are kept. Every
// line is also mirrored to OutputDebugStringA, so DebugView keeps working.
//
// This log exists for exactly one question: "the files are in the right place
// but nothing injected -- how far did it get?" It is therefore VERBOSE
// UNCONDITIONALLY -- there is no level filter and no ini switch, because the
// only person who ever reads it is looking at a boot that already failed, and
// a setting they would have had to turn on beforehand is a setting that was
// off when it mattered. It is a few dozen lines per launch.
//
// Note the asymmetry that makes this file worth having: an EMPTY ModLoader\
// Logs\ folder (no Proxy.log at all) means the game never loaded our
// dwmapi.dll in the first place -- wrong folder, an anti-cheat/overlay
// pre-loading the system copy, or the file blocked/quarantined. Once
// Proxy.log exists, the failure is downstream of that and the log says where.
//
// The proxy still does NOT use LogToFile (logging/log.h, part of
// StarRupture-ModLoader-Core.dll): LogToFile keeps its state in `inline`
// globals, which get a *separate* copy per DLL, so both DLLs calling
// Initialize() would each CREATE_ALWAYS the same modloader.log and clobber
// each other. Core owns modloader.log exclusively; the proxy owns Proxy.log
// exclusively. Different files, no shared state, no conflict.
//
// All functions are safe to call from any thread and before Initialize()
// (messages then go to OutputDebugStringA only) -- logging must never be the
// reason the loader fails.
// ---------------------------------------------------------------------------

namespace ProxyLog
{
    // gameDir: the game's Binaries\Win64 directory (no trailing backslash).
    // Creates ModLoader\ and ModLoader\Logs\ if missing, rotates the previous
    // Proxy.log, and opens the new one. Returns false if the file could not
    // be opened (read-only folder, etc.) -- logging then degrades to
    // OutputDebugStringA only rather than failing the load.
    bool Initialize(const wchar_t* gameDir);

    void Shutdown();

    // Emits the "who/where/what" block: proxy module path + version, host
    // process path and PID, command line, the ModLoader folder contents and
    // whether the expected files are present. This is the part that answers
    // most "it isn't injecting" reports on its own, so it runs before any
    // work that can fail.
    void LogEnvironment(const wchar_t* gameDir);

    void Debug(const char* fmt, ...);
    void Info(const char* fmt, ...);
    void Warn(const char* fmt, ...);
    void Error(const char* fmt, ...);
}
