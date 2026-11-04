// LayoutCheckTests: the startup layout check's comparison, with no game.
//
// fixtures\ holds two layout tables generated from the Game-SDK at CL 126119
// and CL 127004 (ACrCharacterPlayerBase and its reflected bases). One stands
// in for the SDK the loader was built against, the other for the running
// game, so the comparison sees exactly what a loader built for one build
// would see on the other. See Tests/README.md.

#include "compat_report.h"
#include "layout_check.h"
#include "layout_table.generated.h"

#include "fixtures/layout_table_126119.h"
#include "fixtures/layout_table_127004.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    bool Contains(const LayoutCheck::Result& result, const std::string& line)
    {
        return std::find(result.lines.begin(), result.lines.end(), line) != result.lines.end();
    }

    // A table read back as what the game would reflect for each class.
    std::map<std::string, LayoutCheck::LiveStruct> AsGame(const std::vector<LayoutCheck::TableClass>& table)
    {
        std::map<std::string, LayoutCheck::LiveStruct> game;
        for (const LayoutCheck::TableClass& c : table)
        {
            LayoutCheck::LiveStruct& live = game[c.path];
            live.size = static_cast<int32_t>(c.sizeOf);
            live.minAlignment = static_cast<int32_t>(c.alignOf);
            for (const LayoutCheck::TableRow& row : c.rows)
            {
                live.properties.push_back(LayoutCheck::LiveProperty{ row.name, static_cast<int32_t>(row.offset),
                    static_cast<int32_t>(row.size), row.boolMask != 0, row.boolMask });
            }
        }
        return game;
    }

    LayoutCheck::Result CompareWith(const std::vector<LayoutCheck::TableClass>& sdk,
        const std::map<std::string, LayoutCheck::LiveStruct>& game)
    {
        return LayoutCheck::Compare(sdk, [&game](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
        {
            const auto it = game.find(c.path);
            return it == game.end() ? nullptr : &it->second;
        });
    }

    const auto kTable126119 = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable126119::kNames, SrLayoutTable126119::kClasses, SrLayoutTable126119::kRows);
    };
    const auto kTable127004 = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable127004::kNames, SrLayoutTable127004::kClasses, SrLayoutTable127004::kRows);
    };

    void SameBuildMatches()
    {
        std::printf("127004 SDK on a 127004 game\n");
        const auto table = kTable127004();
        const LayoutCheck::Result result = CompareWith(table, AsGame(table));

        Check(result.classes == static_cast<int>(SrLayoutTable127004::kClassCount), "every class is counted");
        Check(result.checked == result.classes, "every class is checked");
        Check(result.mismatched == 0 && result.missing == 0 && result.deferred == 0, "0 mismatched, 0 missing, 0 deferred");
        Check(result.lines.empty(), "nothing to log");
        Check(!LayoutCheck::RequiresSafeOff(result), "plugins stay on");
    }

    void ContentUpdateMovesThePlayer()
    {
        std::printf("126119 SDK on a 127004 game\n");
        const LayoutCheck::Result result = CompareWith(kTable126119(), AsGame(kTable127004()));

        Check(result.changedClasses == std::vector<std::string>{ "ACrCharacterPlayerBase" },
            "ACrCharacterPlayerBase is the one changed class");
        Check(result.mismatched == 1 && result.missing == 0, "1 mismatched, 0 missing");
        Check(Contains(result, "ACrCharacterPlayerBase: size SDK 0x15E0, game 0x15F0"), "its size moved 0x15E0 -> 0x15F0");
        Check(Contains(result, "ACrCharacterPlayerBase::ADSMoveSpeed: offset SDK 0x13A8, game 0x13B8"),
            "ADSMoveSpeed moved 0x13A8 -> 0x13B8");

        for (const std::string& line : result.lines)
        {
            if (line.rfind("ACrCharacterPlayerBase", 0) != 0)
            {
                Check(false, ("only the player class differs: " + line).c_str());
                break;
            }
        }
    }

    // The forced mismatch: a loader built on 126119 running on 127004. The
    // check must switch plugins off, and the report must say what moved.
    void MismatchSwitchesPluginsOffAndReports()
    {
        std::printf("Safe-off and the compatibility report (126119 SDK on a 127004 game)\n");
        const LayoutCheck::Result result = CompareWith(kTable126119(), AsGame(kTable127004()));
        Check(LayoutCheck::RequiresSafeOff(result), "plugins are switched off");
        Check(result.changes.size() == result.lines.size(), "one report entry per log line");

        const CompatReport::Inputs inputs{ "1.0.0.0-CL-127004", "StarRupture-Win64-Shipping.exe", std::string(64, 'a'), 126119,
            { { "Some_Pattern", "48 8B \"05\"", false, true } },
            { { "GObjects", true, 0x1000, 0x1000, false }, { "GWorld", false, 0, 0x2000, false },
              { "GNames", true, 0x3000, 0x4000, true } },
            &result };
        const std::string json = CompatReport::Build(inputs);
        const auto has = [&json](const char* text) { return json.find(text) != std::string::npos; };

        Check(has("\"productVersion\": \"1.0.0.0-CL-127004\""), "the game's ProductVersion");
        Check(has("\"exe\": { \"name\": \"StarRupture-Win64-Shipping.exe\", \"sha256\": \"aaaaaaaa"), "the exe's name and SHA-256");
        Check(has("\"sdkCL\": 126119"), "the SDK's CL");
        Check(has("{ \"name\": \"Some_Pattern\", \"module\": \"StarRupture-Win64-Shipping.exe\", \"pattern\": \"48 8B \\\"05\\\"\", \"required\": false, \"found\": true }"),
            "a missed pattern, quotes escaped");
        Check(has("{ \"name\": \"GObjects\", \"resolved\": true, \"rva\": 4096, \"sdkOffset\": 4096, \"rejected\": false }")
            && has("{ \"name\": \"GWorld\", \"resolved\": false, \"rva\": 0, \"sdkOffset\": 8192, \"rejected\": false }"), "each global's RVA, resolved or not");
        Check(has("{ \"name\": \"GNames\", \"resolved\": true, \"rva\": 12288, \"sdkOffset\": 16384, \"rejected\": true }"),
            "a rejected hit keeps its RVA and is flagged");
        Check(has("{ \"class\": \"ACrCharacterPlayerBase\", \"member\": \"\", \"field\": \"size\", \"sdk\": 5600, \"game\": 5616 }"),
            "the class size, old and new");
        Check(has("{ \"class\": \"ACrCharacterPlayerBase\", \"member\": \"ADSMoveSpeed\", \"field\": \"offset\", \"sdk\": 5032, \"game\": 5048 }"),
            "the moved member, old and new offset");
        Check(json.size() < 100 * 1024, "under 100 KB");

        Check(CompatReport::ClOf("1.0.0.0-CL-127004") == "127004" && CompatReport::ClOf("1.0.0.0") == "unknown"
            && CompatReport::ClOf("1.0-CL-") == "unknown", "the CL for the file name");
    }

    // Dumper-7 comments a static array with the size of one element; the
    // table holds the whole member, as the game reflects it.
    void StaticArrayIsTheWholeMember()
    {
        std::printf("Static arrays\n");
        using LayoutCheck::Kind;

        const auto table = LayoutCheck::ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows);
        bool found = false;
        for (const LayoutCheck::TableClass& c : table)
        {
            for (const LayoutCheck::TableRow& row : c.rows)
            {
                if (std::string(c.name) == "UWorld" && std::string(row.name) == "LineBatchers")
                    found = row.offset == 0xF0 && row.size == 0x20;
            }
        }
        Check(found, "the loader's table has UWorld::LineBatchers[4] as 0x20 bytes at 0xF0");

        // ULineBatchComponent* LineBatchers[4]: ElementSize 8, ArrayDim 4.
        const LayoutCheck::LiveStruct world{ 0x100, 8, { { "LineBatchers", 0xF0, 8 * 4, false, 0 } } };
        const std::map<std::string, LayoutCheck::LiveStruct> game = { { "Engine.World", world } };
        const std::vector<LayoutCheck::TableClass> whole = { { "UWorld", "Engine.World", 0x100, 8, Kind::Class, { { "LineBatchers", 0xF0, 0x20, 0 } } } };
        const std::vector<LayoutCheck::TableClass> element = { { "UWorld", "Engine.World", 0x100, 8, Kind::Class, { { "LineBatchers", 0xF0, 0x8, 0 } } } };

        const LayoutCheck::Result wholeResult = CompareWith(whole, game);
        Check(wholeResult.lines.empty() && !LayoutCheck::RequiresSafeOff(wholeResult), "the whole member matches the game");
        Check(Contains(CompareWith(element, game), "UWorld::LineBatchers: size SDK 0x8, game 0x20"),
            "one element's size is the old false positive");
    }

    void EachDifferenceIsReported()
    {
        std::printf("Synthetic differences\n");
        using LayoutCheck::Kind;
        const std::vector<LayoutCheck::TableClass> sdk = {
            { "AGone", "Mod.Gone", 0x10, 8, Kind::Class, {} },
            { "UBlueprint_C", "Blueprint_C.Blueprint_C", 0x10, 8, Kind::Blueprint, {} },
            { "FUnknown", "Mod.Unknown", 0x10, 8, Kind::Unknown, {} },
            { "Lib_Call", "Mod.Lib.Call", 0x18, 8, Kind::FunctionParams, { { "ReturnValue", 0x10, 8, 0 } } },
            { "FThing", "Mod.Thing", 0x20, 8, Kind::ScriptStruct, {
                { "Missing", 0x0, 4, 0 },
                { "Size", 0x8, 4, 0 },
                { "Bit", 0x10, 1, 0x02 },
                { "WasBool", 0x11, 1, 0xFF },
                { "NowBool", 0x12, 1, 0 },
                { "Unsized", 0x14, 0, 0 },
            } },
        };

        LayoutCheck::LiveStruct thing{ 0x20, 16, {
            { "Size", 0x8, 8, false, 0 },
            { "Bit", 0x10, 1, true, 0x04 },
            { "WasBool", 0x11, 1, false, 0 },
            { "NowBool", 0x12, 1, true, 0xFF },
            { "Unsized", 0x14, 12, false, 0 },
        } };
        // A native UFunction reflects MinAlignment 1 whatever its parameters hold.
        const LayoutCheck::LiveStruct call{ 0x18, 1, { { "ReturnValue", 0x10, 8, false, 0 } } };
        const std::map<std::string, LayoutCheck::LiveStruct> game = { { "Mod.Thing", thing }, { "Mod.Lib.Call", call } };
        const LayoutCheck::Result result = CompareWith(sdk, game);

        Check(result.classes == 5 && result.checked == 2 && result.mismatched == 1 && result.missing == 1
            && result.deferred == 2, "5 classes: 2 checked, 1 mismatched, 1 missing, 2 deferred");
        Check(Contains(result, "AGone (Mod.Gone): not found in the game"), "a missing class");
        Check(Contains(result, "FThing: alignment SDK 8, game 16"), "an alignment change");
        Check(Contains(result, "FThing::Missing: not found in the game"), "a missing member");
        Check(Contains(result, "FThing::Size: size SDK 0x4, game 0x8"), "a member size change");
        Check(Contains(result, "FThing::Bit: bool mask SDK 0x02, game 0x04"), "a bit-field that moved bit");
        Check(Contains(result, "FThing::WasBool: a bool in the SDK, not in the game"), "a bool that is no longer one");
        Check(Contains(result, "FThing::NowBool: a bool in the game, not in the SDK"), "a member that became a bool");
        Check(result.lines.size() == 7, "nothing else: a size of 0 and a function's alignment are not compared");
    }

    void UsedMembersOnly()
    {
        std::printf("Granularity 1: the members the loader reads\n");
        using LayoutCheck::Kind;

        Check(SrLayoutTable::kGranularity == 1, "the loader's table lists only the members it reads");
        const std::vector<LayoutCheck::TableClass> table = LayoutCheck::ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows);
        const auto row = [&table](const char* cls, const char* member) -> const LayoutCheck::TableRow*
        {
            for (const LayoutCheck::TableClass& c : table)
                if (std::string(c.name) == cls)
                    for (const LayoutCheck::TableRow& r : c.rows)
                        if (std::string(r.name) == member) return &r;
            return nullptr;
        };
        const auto sizeOf = [&table](const char* cls) -> uint32_t
        {
            for (const LayoutCheck::TableClass& c : table)
                if (std::string(c.name) == cls) return c.sizeOf;
            return 0;
        };
        Check(row("UChannel", "Connection") && row("UNetConnection", "OpenChannels"),
            "the members control_channel reads through offsetof keep their rows");
        Check(sizeOf("UFunction") != 0 && sizeOf("UWorld") == 0,
            "UFunction, which delegate_hook copies by value, keeps its size; UWorld, reached through pointers, does not");

        // A class that grew, with the member the loader reads in place (size 0
        // in the table: reached through pointers); a class the loader copies
        // that grew; a struct that grew; a class whose read member moved; a
        // member left to the per-build resolver that moved.
        const std::vector<LayoutCheck::TableClass> sdk = {
            { "UGrew", "Mod.Grew", 0, 0, Kind::Class, { { "Read", 0x30, 8, 0 } } },
            { "UCopied", "Mod.Copied", 0x40, 8, Kind::Class, {} },
            { "FGrew", "Mod.GrewStruct", 0x10, 8, Kind::ScriptStruct, { { "A", 0x0, 8, 0 } } },
            { "UMoved", "Mod.Moved", 0x40, 8, Kind::Class, { { "Read", 0x30, 8, 0 } } },
            { "UPerBuild", "Mod.PerBuild", 0x40, 8, Kind::Class, { { "Resolved", 0x30, 8, 0 }, { "Read", 0x38, 8, 0 } } },
        };
        const std::map<std::string, LayoutCheck::LiveStruct> game = {
            { "Mod.Grew", { 0x48, 16, { { "Read", 0x30, 8, false, 0 } } } },
            { "Mod.Copied", { 0x48, 8, {} } },
            { "Mod.GrewStruct", { 0x18, 8, { { "A", 0x0, 8, false, 0 } } } },
            { "Mod.Moved", { 0x40, 8, { { "Read", 0x38, 8, false, 0 } } } },
            { "Mod.PerBuild", { 0x40, 8, { { "Resolved", 0x48, 8, false, 0 }, { "Read", 0x38, 8, false, 0 } } } },
        };
        const auto find = [&game](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
        {
            const auto it = game.find(c.path);
            return it == game.end() ? nullptr : &it->second;
        };
        const auto perBuild = [](const LayoutCheck::TableClass& c, const LayoutCheck::TableRow& row)
        {
            return std::string(c.name) == "UPerBuild" && std::string(row.name) == "Resolved";
        };

        const LayoutCheck::Result narrow = LayoutCheck::Compare(sdk, find, perBuild);
        Check(!Contains(narrow, "UGrew: size") && !Contains(narrow, "UGrew: alignment"),
            "a size and alignment of 0 are not compared");
        Check(Contains(narrow, "UCopied: size SDK 0x40, game 0x48"), "a UClass that keeps its size in the table is compared");
        Check(Contains(narrow, "FGrew: size SDK 0x10, game 0x18"), "a script struct's size still is");
        Check(Contains(narrow, "UMoved::Read: offset SDK 0x30, game 0x38"), "a member the loader reads that moved switches plugins off");
        Check(narrow.perBuild == 1 && !Contains(narrow, "UPerBuild::Resolved: offset SDK 0x30, game 0x48"),
            "a member resolved per build is left to the version map");
        Check(narrow.mismatched == 3 && narrow.lines.size() == 3 && LayoutCheck::RequiresSafeOff(narrow), "nothing else");

        const LayoutCheck::Result whole = LayoutCheck::Compare(sdk, find);
        Check(Contains(whole, "UPerBuild::Resolved: offset SDK 0x30, game 0x48"), "without a per-build rule every row is compared");
    }
}

int main()
{
    SameBuildMatches();
    ContentUpdateMovesThePlayer();
    MismatchSwitchesPluginsOffAndReports();
    StaticArrayIsTheWholeMember();
    EachDifferenceIsReported();
    UsedMembersOnly();

    if (g_failures)
        std::printf("\n%d check(s) FAILED\n", g_failures);
    else
        std::printf("\nAll checks passed\n");
    return g_failures ? 1 : 0;
}
