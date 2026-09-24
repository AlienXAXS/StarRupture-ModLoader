#include "memory_scanner/pattern_preflight.h"
#include "memory_scanner/scan_validation.h"
#include "hooks/game/scan_patterns.h"
#include "logging/logger.h"
#include "UI/splash_window.h"

#include <vector>
#include <string>

namespace
{
    // Downgrades a uniqueness/kind violation in the loader's OWN registry from
    // fatal to a logged warning. It exists for exactly one situation: a game
    // update has made one of our patterns ambiguous, the user is mid-session and
    // the alternative is a loader that will not start at all until a new build
    // ships.
    //
    // It deliberately does NOT exist for plugins. A plugin refusal costs that
    // plugin; a loader refusal costs the whole mod loader, and those are not the
    // same trade. It is also a command-line switch rather than an ini key on
    // purpose -- an ini key set six months ago and forgotten is how a safety
    // check ends up permanently off.
    //
    // A pattern that does not resolve AT ALL is still fatal with this set. The
    // switch relaxes "is it the right one", never "is it there".
    bool SkipValidationRequested()
    {
        static int s_cached = -1;
        if (s_cached >= 0)
            return s_cached != 0;

        s_cached = 0;
        if (const wchar_t* cmdLine = GetCommandLineW())
        {
            if (wcsstr(cmdLine, L"-SkipPatternValidation") != nullptr)
                s_cached = 1;
        }

        return s_cached != 0;
    }

    ScanValidation::Kind ToValidationKind(ScanPatterns::PatternKind kind)
    {
        return static_cast<ScanValidation::Kind>(static_cast<int>(kind));
    }

    // One failed entry, kept so the summary can list them all together rather
    // than leaving the reader to reassemble it from sixty interleaved lines.
    struct Failure
    {
        const char* name;
        std::string detail;
        bool        structural; // true = found but wrong/ambiguous, false = not found
    };

    void LogDetail(const std::string& detail, bool asError)
    {
        // The detail blocks are CRLF-separated and deliberately multi-line --
        // splitting them keeps the log's one-line-per-entry format intact
        // instead of emitting a single line with literal CRLFs in it.
        size_t start = 0;
        while (start <= detail.size())
        {
            size_t end = detail.find("\r\n", start);
            const std::string line = detail.substr(start, end == std::string::npos ? std::string::npos : end - start);

            if (!line.empty())
            {
                if (asError)
                    ModLoaderLogger::LogError(L"[Preflight]   %S", line.c_str());
                else
                    ModLoaderLogger::LogWarn(L"[Preflight]   %S", line.c_str());
            }

            if (end == std::string::npos)
                break;
            start = end + 2;
        }
    }
}

namespace PatternPreflight
{
    bool VerifyAllPatterns(std::wstring* outFailureDetails)
    {
        constexpr size_t total = sizeof(ScanPatterns::PreflightRegistry) / sizeof(ScanPatterns::PreflightRegistry[0]);

        const bool skipValidation = SkipValidationRequested();

        ModLoaderLogger::LogInfo(L"[Preflight] Verifying %zu modloader scan patterns...", total);
        if (skipValidation)
        {
            ModLoaderLogger::LogWarn(L"[Preflight] -SkipPatternValidation is set: ambiguous or wrong-kind patterns");
            ModLoaderLogger::LogWarn(L"[Preflight] will be logged and USED anyway. A pattern that is missing entirely");
            ModLoaderLogger::LogWarn(L"[Preflight] is still fatal. Remove the switch once the patterns are fixed.");
        }

        std::vector<Failure> fatal;
        std::vector<Failure> nonFatal;
        size_t found = 0;

        size_t index = 0;
        for (const auto& entry : ScanPatterns::PreflightRegistry)
        {
            ++index;
            wchar_t status[256]{};
            swprintf_s(status, L"Verifying scan patterns (%zu/%zu): %S", index, total, entry.name);
            Splash::SetStatus(status);
            Splash::SetProgress(static_cast<float>(index) / static_cast<float>(total));

            ScanValidation::Request request;
            request.name           = entry.name;
            request.pattern        = entry.pattern;
            request.kind           = ToValidationKind(entry.kind);
            request.followRel32At  = entry.followRel32At;

            const ScanValidation::Result result = ScanValidation::Resolve(request);

            if (result.Succeeded())
            {
                ++found;
                continue;
            }

            const bool structural = (result.outcome != ScanValidation::Outcome::NoMatch);

            // The escape hatch covers "found, but not provably the right one".
            // It cannot cover "not found" -- there is no address to fall back to.
            const bool tolerated = skipValidation && structural;
            const bool isFatal   = entry.required && !tolerated;

            Failure failure{ entry.name, result.detail, structural };

            if (isFatal)
            {
                ModLoaderLogger::LogError(L"[Preflight] REQUIRED pattern FAILED: %S", entry.name);
                LogDetail(result.detail, true);
                fatal.push_back(std::move(failure));
            }
            else
            {
                ModLoaderLogger::LogWarn(L"[Preflight] Non-fatal pattern problem: %S", entry.name);
                LogDetail(result.detail, false);
                nonFatal.push_back(std::move(failure));

                if (tolerated)
                {
                    ++found; // it resolved, we are just not vouching for it
                    ModLoaderLogger::LogWarn(L"[Preflight]   USED ANYWAY because -SkipPatternValidation is set");
                }
            }
        }

        ModLoaderLogger::LogInfo(
            L"[Preflight] Pattern verification complete: %zu/%zu usable (%zu fatal, %zu non-fatal)",
            found, total, fatal.size(), nonFatal.size());

        if (fatal.empty())
            return true;

        ModLoaderLogger::LogError(L"[Preflight] ==========================================================");
        ModLoaderLogger::LogError(L"[Preflight] %zu REQUIRED scan pattern(s) could not be verified:", fatal.size());
        for (const Failure& f : fatal)
        {
            ModLoaderLogger::LogError(L"[Preflight]   - %S  (%S)", f.name,
                f.structural ? "found, but not what it was declared to be" : "not found");
        }
        ModLoaderLogger::LogError(L"[Preflight] The game likely updated and the patterns need refreshing.");
        ModLoaderLogger::LogError(L"[Preflight] The mod loader will DISABLE itself: no hooks will be");
        ModLoaderLogger::LogError(L"[Preflight] installed and no plugins will be loaded. The game will");
        ModLoaderLogger::LogError(L"[Preflight] start completely unmodified.");
        ModLoaderLogger::LogError(L"[Preflight] ==========================================================");

        if (outFailureDetails)
        {
            wchar_t line[512]{};
            swprintf_s(line, L"%zu of %zu required scan pattern(s) could not be verified:\r\n\r\n",
                fatal.size(), total);
            *outFailureDetails += line;

            for (const Failure& f : fatal)
            {
                swprintf_s(line, L"  - %S\r\n      %S\r\n", f.name,
                    f.structural ? "found, but not what it was declared to be" : "not found in the game executable");
                *outFailureDetails += line;
            }

            *outFailureDetails +=
                L"\r\nThe game likely updated and the ModLoader's patterns need refreshing.\r\n"
                L"Full details (every match and what it landed on) are in ModLoader.log.\r\n";

            // Only offer the switch when it could actually help. Suggesting it
            // for a pattern that is simply gone would send someone to try
            // something that cannot work.
            bool anyStructural = false;
            for (const Failure& f : fatal)
                anyStructural = anyStructural || f.structural;

            if (anyStructural)
            {
                *outFailureDetails +=
                    L"\r\nIf you need to start anyway, launch with -SkipPatternValidation. The\r\n"
                    L"mod loader will then use patterns it cannot verify, which may be unstable.\r\n";
            }
        }

        return false;
    }
}
