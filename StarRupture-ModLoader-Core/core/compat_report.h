#pragma once

#include "layout_check.h"

#include <cstdint>
#include <string>
#include <vector>

// The compatibility report: ModLoader\compat-report-<CL>.json, written when
// the startup layout check switches something off, and again, with the same
// layout findings, if a later startup step switches every plugin off. It is
// also written, with outcome "unchanged", on a boot where the layout check
// found nothing but a plugin asked for a struct size, so the sizes are always
// on record. It says
// which game build this is, whether only some loader features and plugins went
// off or every plugin did, which scan patterns missed, where the engine globals
// resolved, which SDK class members moved, and for each struct size a plugin
// asked for the map's row next to the size the game reflects, so a loader
// update can be made from it.
// Nothing else goes in: no paths, no user or machine names. A plugin's name
// appears only as the owner of a size request.
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

    // One size request a plugin made (VersionMap::SizeFindings). rowCL and
    // rowSize are 0 when there was no row, reflectedSize is meaningful only
    // when reflected is set; the report writes null for each that is missing.
    struct SizeKey
    {
        std::string plugin;
        std::string key;
        uint32_t    rowCL;
        uint32_t    rowSize;
        bool        reflected;
        uint32_t    reflectedSize;
        std::string result;     // match, mismatch, row-only, no-row or reflected-only
    };

    // What the boot switched off.
    enum class Outcome
    {
        Partial,  // only what reads the changed classes (SwitchOffChangedLayoutUsers)
        SafeOff,  // every plugin (EnterLayoutSafeOff)
        Unchanged,  // nothing switched off; written only for the sizes plugins asked for
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
        std::vector<SizeKey>       sizes;
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

    // Whether Refresh writes: when a report was already written this session
    // (so it gets the sizes plugins asked for since), or when no layout change
    // wrote one but a plugin asked for a size.
    bool RefreshWrites(bool alreadyWritten, size_t sizeRequests);

    // Plugins ask for their struct sizes after the first Write. Called once
    // they are initialised, with the layout check's result.
    void Refresh(const LayoutCheck::Result& layout);
}
