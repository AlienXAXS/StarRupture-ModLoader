#pragma once

#include "layout_check.h"

#include <cstdint>
#include <string>
#include <vector>

// The compatibility report: ModLoader\compat-report-<CL>.json, written when
// the startup layout check switches something off, and again, with the same
// layout findings, if a later startup step switches every plugin off. It says
// which game build this is, whether only some loader features and plugins went
// off or every plugin did, which scan patterns missed, where the engine globals
// resolved and which SDK class members moved, so a loader update can be made
// from it.
// Nothing else goes in: no paths, no user or machine names, no plugin list.
namespace CompatReport
{
    struct PatternMiss
    {
        std::string name;
        std::string pattern;
        bool        required;
        bool        found;      // the pattern matched, but not provably the right place
    };

    struct GlobalRva
    {
        std::string name;
        bool        resolved;
        uint64_t    rva;        // 0 when not resolved; kept when rejected
        int32_t     sdkOffset;  // the SDK's Offsets:: value
        bool        rejected;   // resolved, but the loader used sdkOffset instead
    };

    // What the boot switched off.
    enum class Outcome
    {
        Partial,  // only what reads the changed classes (SwitchOffChangedLayoutUsers)
        SafeOff,  // every plugin (EnterLayoutSafeOff)
    };

    struct Inputs
    {
        std::string                productVersion;  // the game exe's ProductVersion
        std::string                exeName;         // file name only
        std::string                exeSha256;       // lowercase hex; empty if it could not be read
        uint32_t                   sdkCL;           // the CL of the SDK the loader was built against
        std::vector<PatternMiss>   patternMisses;
        std::vector<GlobalRva>     globals;
        const LayoutCheck::Result* layout;
        Outcome                    outcome;
    };

    // The report's JSON text. Kept apart from the game so it can be tested.
    std::string Build(const Inputs& inputs);

    // The CL in a ProductVersion ("1.0.0-CL-127004" -> "127004"), or
    // "unknown" when it has none.
    std::string ClOf(const std::string& productVersion);

    // Client builds: gathers the inputs, writes the report and returns its
    // file name (empty if it could not be written). Called from the init
    // thread. A later call replaces the file, so it carries everything the
    // earlier one did.
    std::wstring Write(const LayoutCheck::Result& layout, Outcome outcome);
}
