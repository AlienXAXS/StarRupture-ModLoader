#pragma once
#include <string>

// Returns true if the running game executable is the build this ModLoader was
// compiled against. On mismatch it logs the details and, if outDetails is
// non-null, fills it with a human-readable expected/detected summary (CRLF line
// endings) for the crash dialog's details box. It shows no UI itself -- the
// caller decides what to do while the game main thread is still held.
bool VerifyGameVersion(std::wstring* outDetails = nullptr);

// Returns the raw ProductVersion string read from the game executable
// (e.g. "1.2.3-CL-120722"), or an empty string if it could not be read.
// Used as the cache-invalidation key for the pattern scan offset cache.
std::wstring GetGameVersionString();
