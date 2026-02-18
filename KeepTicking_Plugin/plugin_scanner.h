#pragma once

#include "plugin_interface.h"

// Wrapper around IPluginScanner for convenient usage throughout the plugin
namespace PluginScanner
{
    // Initialize with the scanner interface from the loader
    void Initialize(IPluginScanner* scanner);

    // Find pattern in main module - returns absolute address or 0 if not found
    uintptr_t FindPatternInMainModule(const char* pattern);

    // Find pattern in specific module - returns absolute address or 0 if not found
 uintptr_t FindPatternInModule(HMODULE module, const char* pattern);

    // Find all occurrences in main module
    std::vector<uintptr_t> FindAllPatternsInMainModule(const char* pattern);

    // Find all occurrences in specific module
    std::vector<uintptr_t> FindAllPatternsInModule(HMODULE module, const char* pattern);

    // Find unique pattern from candidates
    uintptr_t FindUniquePattern(const char** patterns, int patternCount, int* outPatternIndex = nullptr);
}
