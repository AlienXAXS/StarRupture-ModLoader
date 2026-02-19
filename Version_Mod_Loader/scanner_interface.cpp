#include "scanner_interface.h"
#include "scanner.h"
#include <vector>
#include <string>

namespace ModLoader
{
    // Wrapper functions that convert C-style calls to C++ Scanner namespace calls
    
    static uintptr_t ScannerFindPatternInMainModule(const char* pattern)
    {
     if (!pattern) return 0;
   return Scanner::FindPatternInMainModule(std::string(pattern));
    }

    static uintptr_t ScannerFindPatternInModule(HMODULE module, const char* pattern)
    {
        if (!pattern) return 0;
        return Scanner::FindPatternInModule(module, std::string(pattern));
    }

    static int ScannerFindAllPatternsInMainModule(const char* pattern, uintptr_t* outAddresses, int maxResults)
    {
        if (!pattern) return 0;
        std::vector<uintptr_t> results = Scanner::FindAllPatternsInMainModule(std::string(pattern));
        int count = static_cast<int>(results.size());
        if (outAddresses && maxResults > 0)
        {
            int toCopy = count < maxResults ? count : maxResults;
            for (int i = 0; i < toCopy; ++i)
                outAddresses[i] = results[i];
        }
        return count;
    }

    static int ScannerFindAllPatternsInModule(HMODULE module, const char* pattern, uintptr_t* outAddresses, int maxResults)
    {
        if (!pattern) return 0;
        std::vector<uintptr_t> results = Scanner::FindAllPatternsInModule(module, std::string(pattern));
        int count = static_cast<int>(results.size());
        if (outAddresses && maxResults > 0)
        {
            int toCopy = count < maxResults ? count : maxResults;
            for (int i = 0; i < toCopy; ++i)
                outAddresses[i] = results[i];
        }
        return count;
    }

    static uintptr_t ScannerFindUniquePattern(const char** patterns, int patternCount, int* outPatternIndex)
    {
     if (!patterns || patternCount <= 0) return 0;

        // Convert C-style array to std::vector<std::string>
        std::vector<std::string> patternVec;
        patternVec.reserve(patternCount);
        for (int i = 0; i < patternCount; ++i)
        {
            if (patterns[i])
      patternVec.push_back(std::string(patterns[i]));
     }

        return Scanner::FindUniquePattern(patternVec, outPatternIndex);
    }

    // Global scanner interface instance
    static IPluginScanner g_pluginScanner = {
        ScannerFindPatternInMainModule,
      ScannerFindPatternInModule,
        ScannerFindAllPatternsInMainModule,
        ScannerFindAllPatternsInModule,
   ScannerFindUniquePattern
    };

    IPluginScanner* GetPluginScanner()
    {
        return &g_pluginScanner;
    }
}
