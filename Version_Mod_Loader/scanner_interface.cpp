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

    // Helper: convert Scanner::XRef vector to caller-supplied PluginXRef buffer
    static int CopyXRefsToBuffer(const std::vector<Scanner::XRef>& xrefs,
                                  PluginXRef* outXRefs, int maxResults)
    {
        int count = static_cast<int>(xrefs.size());
        if (outXRefs && maxResults > 0)
        {
            int toCopy = count < maxResults ? count : maxResults;
            for (int i = 0; i < toCopy; ++i)
            {
                outXRefs[i].address    = xrefs[i].address;
                outXRefs[i].isRelative = xrefs[i].isRelative;
            }
        }
        return count;
    }

    static int ScannerFindXrefsToAddress(uintptr_t targetAddress, uintptr_t start, size_t size,
                                          PluginXRef* outXRefs, int maxResults)
    {
        auto xrefs = Scanner::FindXrefsToAddress(targetAddress, start, size);
        return CopyXRefsToBuffer(xrefs, outXRefs, maxResults);
    }

    static int ScannerFindXrefsToAddressInModule(uintptr_t targetAddress, HMODULE module,
                                                  PluginXRef* outXRefs, int maxResults)
    {
        auto xrefs = Scanner::FindXrefsToAddressInModule(targetAddress, module);
        return CopyXRefsToBuffer(xrefs, outXRefs, maxResults);
    }

    static int ScannerFindXrefsToAddressInMainModule(uintptr_t targetAddress,
                                                      PluginXRef* outXRefs, int maxResults)
    {
        auto xrefs = Scanner::FindXrefsToAddressInMainModule(targetAddress);
        return CopyXRefsToBuffer(xrefs, outXRefs, maxResults);
    }

    // Global scanner interface instance
    static IPluginScanner g_pluginScanner = {
        ScannerFindPatternInMainModule,
      ScannerFindPatternInModule,
        ScannerFindAllPatternsInMainModule,
        ScannerFindAllPatternsInModule,
        ScannerFindUniquePattern,
        ScannerFindXrefsToAddress,
        ScannerFindXrefsToAddressInModule,
        ScannerFindXrefsToAddressInMainModule
    };

    IPluginScanner* GetPluginScanner()
    {
        return &g_pluginScanner;
    }
}
