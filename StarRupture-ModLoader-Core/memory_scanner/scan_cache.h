#pragma once

#include <cstdint>
#include <string>

// Persists pattern-scan results (offset from main module base) to disk, keyed
// by the game's ProductVersion string and a hash of the AOB pattern string.
// On a matching version, callers can skip the (slow) byte-by-byte scan
// entirely and jump straight to the cached offset.
//
// The pattern string itself (not the caller-supplied name) is hashed to form
// the key: plugin callers don't supply a friendly name -- they pass the same
// AOB string for both -- and the pattern bytes are what actually determine
// whether a cached offset is reusable.
//
// Cache entries are always re-validated against the live process bytes before
// use (see Scanner::FindPatternInMainModule), so a stale or hand-edited cache
// can never produce a wrong address -- at worst it just falls back to a full scan.
namespace ScanCache
{
    // Looks up a cached offset (relative to the main module base) for the given
    // AOB pattern string under the given game version. Returns true and fills
    // outOffset on a hit, false on a miss.
    bool TryGetOffset(const std::wstring& gameVersion, const std::string& pattern, uintptr_t& outOffset);

    // Stores/updates the cached offset for the given AOB pattern string under
    // the given game version and writes the cache file to disk immediately.
    void StoreOffset(const std::wstring& gameVersion, const std::string& pattern, uintptr_t offset);
}
