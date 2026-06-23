#pragma once
#include <string>

bool VerifyGameVersion();

// Returns the raw ProductVersion string read from the game executable
// (e.g. "1.2.3-CL-120722"), or an empty string if it could not be read.
// Used as the cache-invalidation key for the pattern scan offset cache.
std::wstring GetGameVersionString();
