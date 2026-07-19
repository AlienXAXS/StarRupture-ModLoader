#include "memory_scanner/pattern_preflight.h"
#include "memory_scanner/scanner.h"
#include "hooks/game/scan_patterns.h"
#include "logging/logger.h"

#include <vector>
#include <string>

namespace PatternPreflight
{
    bool VerifyAllPatterns(std::wstring* outFailureDetails)
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

            if (outFailureDetails)
            {
                wchar_t line[512]{};
                swprintf_s(line, L"%zu of %zu required scan pattern(s) could not be found:\r\n\r\n",
                    missingRequired.size(), total);
                *outFailureDetails += line;
                for (const char* name : missingRequired)
                {
                    swprintf_s(line, L"  - %S\r\n", name);
                    *outFailureDetails += line;
                }
                *outFailureDetails +=
                    L"\r\nThe game likely updated and the ModLoader's patterns need refreshing.\r\n"
                    L"Full details (including the pattern bytes) are in ModLoader.log.\r\n";
            }
            return false;
        }

        return true;
    }
}
