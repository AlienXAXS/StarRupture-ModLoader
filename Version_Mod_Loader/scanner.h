#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <vector>
#include <string>

// Pattern scanner for finding byte sequences in process memory.
//
// Patterns use the standard IDA-style format:
//   "48 89 5C 24 ?? 57 48 83 EC 20"
//   where "??" is a wildcard that matches any byte.

namespace Scanner
{
    struct PatternByte
    {
        uint8_t value;
        bool    wildcard;
    };

    // Parse an IDA-style pattern string into a vector of PatternByte.
    std::vector<PatternByte> ParsePattern(const std::string& pattern);

    // Scan a specific memory range for a pattern. Returns the address of the
    // first match, or nullptr if not found.
    uintptr_t FindPattern(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern);

    // Find ALL matches of a pattern in a memory range.
    // Returns a vector of addresses where the pattern was found.
    std::vector<uintptr_t> FindAllPatterns(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern);

    // Scan the main executable module (.exe) for a pattern.
    uintptr_t FindPatternInModule(HMODULE module, const std::string& pattern);

    // Find all matches in a module.
    std::vector<uintptr_t> FindAllPatternsInModule(HMODULE module, const std::string& pattern);

    // Convenience: scan the main process module.
    uintptr_t FindPatternInMainModule(const std::string& pattern);

    // Convenience: find all matches in main module.
    std::vector<uintptr_t> FindAllPatternsInMainModule(const std::string& pattern);

    // Find a unique pattern from a list of candidates.
    // Returns the address if exactly one pattern matches exactly once.
    // Returns 0 if no patterns match uniquely.
    // outPatternIndex is set to the index of the matching pattern (if found).
    uintptr_t FindUniquePattern(const std::vector<std::string>& patterns, int* outPatternIndex = nullptr);
}
