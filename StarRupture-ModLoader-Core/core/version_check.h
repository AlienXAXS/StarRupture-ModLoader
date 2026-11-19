#pragma once
#include <string>

enum class GameVersionVerdict
{
    // The running game executable is a build this ModLoader runs on
    // (VersionMap::SupportedBuilds).
    Known,
    // Client only: the build is not in the version object map, and the
    // ModLoader runs on the newest known build's values, each checked against
    // the game before use (core/unknown_build.h). The session is marked in
    // modloader.ini.
    UnknownFallback,
    // Client only: the last session on this unknown build did not end
    // cleanly, so nothing is hooked this time. outDetails holds the guard
    // dialog's text.
    Guarded,
    // Not a build the ModLoader runs on, and no fallback applies (the server,
    // an exe with no CL, [UnknownBuild] Enabled=0, or a marker that could not
    // be written).
    Refused,
};

// Decides whether the ModLoader runs on the running game executable. On
// Refused it logs the details and, if outDetails is non-null, fills it with a
// human-readable expected/detected summary (CRLF line endings) for the crash
// dialog's details box; on Guarded, with the guard dialog's details. It shows
// no UI itself -- the caller decides what to do while the game main thread is
// still held.
GameVersionVerdict VerifyGameVersion(std::wstring* outDetails = nullptr);

// Returns the raw ProductVersion string read from the game executable
// (e.g. "1.2.3-CL-120722"), or an empty string if it could not be read.
// Used as the cache-invalidation key for the pattern scan offset cache.
std::wstring GetGameVersionString();
