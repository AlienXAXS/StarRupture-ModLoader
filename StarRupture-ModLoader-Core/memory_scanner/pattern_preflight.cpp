#include "memory_scanner/pattern_preflight.h"
#include "memory_scanner/scanner.h"
#include "hooks/game/scan_patterns.h"
#include "logging/logger.h"

#include <vector>

namespace PatternPreflight
{
    bool VerifyAllPatterns()
    {
        constexpr size_t total = sizeof(ScanPatterns::PreflightRegistry) / sizeof(ScanPatterns::PreflightRegistry[0]);

        ModLoaderLogger::LogInfo(L"[Preflight] Verifying %zu modloader scan patterns...", total);

        std::vector<const char*> missingRequired;
        std::vector<const char*> missingOptional;
        size_t found = 0;

        for (const auto& entry : ScanPatterns::PreflightRegistry)
        {
            const uintptr_t addr = Scanner::FindPatternInMainModule(entry.name, entry.pattern);
            if (addr)
            {
                ++found;
                continue;
            }

            if (entry.required)
            {
                missingRequired.push_back(entry.name);
                ModLoaderLogger::LogError(L"[Preflight] REQUIRED pattern NOT FOUND: %S", entry.name);
                ModLoaderLogger::LogError(L"[Preflight]   Pattern: %S", entry.pattern);
            }
            else
            {
                missingOptional.push_back(entry.name);
                ModLoaderLogger::LogWarn(L"[Preflight] Reference-only pattern not found (non-fatal): %S", entry.name);
                ModLoaderLogger::LogWarn(L"[Preflight]   Pattern: %S", entry.pattern);
            }
        }

        ModLoaderLogger::LogInfo(L"[Preflight] Pattern verification complete: %zu/%zu found (%zu required missing, %zu reference-only missing)",
            found, total, missingRequired.size(), missingOptional.size());

        if (!missingRequired.empty())
        {
            ModLoaderLogger::LogError(L"[Preflight] ==========================================================");
            ModLoaderLogger::LogError(L"[Preflight] %zu REQUIRED scan pattern(s) could not be found:",
                missingRequired.size());
            for (const char* name : missingRequired)
                ModLoaderLogger::LogError(L"[Preflight]   - %S", name);
            ModLoaderLogger::LogError(L"[Preflight] The game likely updated and the patterns need refreshing.");
            ModLoaderLogger::LogError(L"[Preflight] The mod loader will DISABLE itself: no hooks will be");
            ModLoaderLogger::LogError(L"[Preflight] installed and no plugins will be loaded. The game will");
            ModLoaderLogger::LogError(L"[Preflight] start completely unmodified.");
            ModLoaderLogger::LogError(L"[Preflight] ==========================================================");
            return false;
        }

        return true;
    }
}
