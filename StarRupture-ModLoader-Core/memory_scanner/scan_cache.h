#pragma once

#include <cstdint>
#include <string>

// Persists pattern-scan results to disk, keyed by the game's ProductVersion
// string and a hash of the AOB pattern string. On a matching version, callers
// can skip the (slow) full-module scan entirely and jump straight to the cached
// offset.
//
// The pattern string itself (not the caller-supplied name) is hashed to form
// the key: plugin callers don't supply a friendly name -- they pass the same
// AOB string for both -- and the pattern bytes are what actually determine
// whether a cached offset is reusable.
//
// An entry carries two numbers:
//
//   offset      -- where the (first) match was, relative to the main module base.
//   matchCount  -- how many times the pattern matched the whole image, or 0 for
//                  "not counted by whoever wrote this entry".
//
// matchCount exists because every resolve now has to prove its pattern is
// unique, and a uniqueness check cannot early-out: without a cached count, the
// strict path would throw the cache away and full-scan on every launch. Storing
// it keeps warm launches as cheap as they were.
//
// Trust rules, which are the point of the split:
//   * The offset is ALWAYS re-validated against the live process bytes before
//     use, so a stale or hand-edited cache can never produce a wrong address --
//     at worst it falls back to a full scan.
//   * A cached matchCount of 1 is accepted after that byte re-validation (the
//     image is identical for a given game version, so a count recorded for that
//     version is as valid as the offset).
//   * A cached matchCount that is anything OTHER than 1 -- including 0/unknown
//     -- forces a full re-scan. A refusal verdict is therefore never based on
//     the cache; only a pass can be, and only after the bytes were checked.
namespace ScanCache
{
    struct Entry
    {
        uintptr_t offset     = 0;
        uint32_t  matchCount = 0;  // 0 = unknown / not counted
    };

    // Looks up the cached entry for the given AOB pattern string under the given
    // game version. Returns true and fills out on a hit, false on a miss.
    bool TryGet(const std::wstring& gameVersion, const std::string& pattern, Entry& out);

    // Stores/updates the entry and writes the cache file to disk immediately.
    void Store(const std::wstring& gameVersion, const std::string& pattern, const Entry& entry);
}
