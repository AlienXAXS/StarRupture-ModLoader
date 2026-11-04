#pragma once

#include <cstdint>
#include <cwchar>
#include <string>

// Persists pattern-scan results to disk, keyed by the game's ProductVersion
// string, the exe's PE timestamp and image size, and a hash of the AOB pattern
// string. On a matching exe, callers can skip the (slow) full-module scan
// entirely and jump straight to the cached offset.
//
// The ProductVersion alone does not say which exe it is: a hotfix that keeps
// the CL, or a patched exe, has the same string and different bytes. The PE
// timestamp and image size are what the version map already uses to tell
// builds apart, so the cache uses them too. A file written before that (one
// section per bare ProductVersion) matches nothing and is discarded.
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
//     image is identical for a given exe, so a count recorded for that exe is
//     as valid as the offset).
//   * A cached matchCount that is anything OTHER than 1 -- including 0/unknown
//     -- forces a full re-scan. A refusal verdict is therefore never based on
//     the cache; only a pass can be, and only after the bytes were checked.
//
// Keys that start with "vmap " are not patterns: VersionMap (version_map.cpp)
// stores the locator its agreement rule picked there. In those entries
// matchCount is 1 + that locator's index and offset is the RVA it gave;
// the pattern path never reads them. Keys that start with "own " are the
// match count of a plugin's own pattern that is not unique: matchCount is 1 +
// that count (offset 0), which VersionMap reads to repeat its warning without
// a second scan.
namespace ScanCache
{
    // The INI section a build's entries live under: the ProductVersion, then
    // the PE timestamp and image size, e.g. "1.0.0-CL-127004|65A1B2C3|3F40000".
    inline std::wstring SectionName(const std::wstring& gameVersion, uint32_t imageStamp, uint32_t imageSize)
    {
        wchar_t suffix[32]{};
        swprintf_s(suffix, L"|%08X|%X", imageStamp, imageSize);
        return gameVersion + suffix;
    }

    struct Entry
    {
        uintptr_t offset     = 0;
        uint32_t  matchCount = 0;  // 0 = unknown / not counted
    };

    // Looks up the cached entry for the given AOB pattern string under the
    // running exe (the given ProductVersion plus the main module's PE timestamp
    // and image size). Returns true and fills out on a hit, false on a miss.
    bool TryGet(const std::wstring& gameVersion, const std::string& pattern, Entry& out);

    // Stores/updates the entry and writes the cache file to disk immediately.
    void Store(const std::wstring& gameVersion, const std::string& pattern, const Entry& entry);
}
