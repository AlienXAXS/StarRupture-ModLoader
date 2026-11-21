// LayoutCheckTests: the startup layout check's comparison, with no game.
//
// fixtures\ holds two layout tables generated from the Game-SDK at CL 126119
// and CL 127004 (ACrCharacterPlayerBase and its reflected bases). One stands
// in for the SDK the loader was built against, the other for the running
// game, so the comparison sees exactly what a loader built for one build
// would see on the other. See Tests/README.md.

#include "compat_report.h"
#include "layout_check.h"
#include "layout_tables.h"
#include "startup_wait.h"

#include "fixtures/layout_table_126119.h"
#include "fixtures/layout_table_127004.h"
#include "fixtures/layout_table_loader_127004.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
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

    // The loader's table as v1.26.1 shipped it (CL 127004), with what reads each class.
    const auto kLoaderTable127004 = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable127004Loader::kNames, SrLayoutTable127004Loader::kClasses,
            SrLayoutTable127004Loader::kRows, SrLayoutTable127004Loader::kConsumers, SrLayoutTable127004Loader::kClassConsumers);
    };

    // The loader's own table, with what reads each class.
    const auto kLoaderTable = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable_130045::kNames, SrLayoutTable_130045::kClasses, SrLayoutTable_130045::kRows,
            SrLayoutTable_130045::kConsumers, SrLayoutTable_130045::kClassConsumers);
    };

    const LayoutCheck::TableClass* ClassNamed(const std::vector<LayoutCheck::TableClass>& table, const char* name)
    {
        for (const LayoutCheck::TableClass& c : table)
            if (std::string(c.name) == name)
                return &c;
        return nullptr;
    }

    // The classes of `table` that `reference` also lists, each with only the
    // members `reference` lists for it (matched by reflection path and member name).
    std::vector<LayoutCheck::TableClass> OnlyWhatIn(const std::vector<LayoutCheck::TableClass>& table,
        const std::vector<LayoutCheck::TableClass>& reference)
    {
        std::vector<LayoutCheck::TableClass> kept;
        for (const LayoutCheck::TableClass& c : table)
            for (const LayoutCheck::TableClass& r : reference)
                if (std::string(c.path) == r.path)
                {
                    LayoutCheck::TableClass copy = c;
                    copy.rows.clear();
                    for (const LayoutCheck::TableRow& row : c.rows)
                        for (const LayoutCheck::TableRow& listed : r.rows)
                            if (std::string(row.name) == listed.name)
                            {
                                copy.rows.push_back(row);
                                break;
                            }
                    kept.push_back(copy);
                    break;
                }
        return kept;
    }

    void SameBuildMatches()
    {
        std::printf("127004 SDK on a 127004 game\n");
        const auto table = kTable127004();
        const LayoutCheck::Result result = CompareWith(table, AsGame(table));

        Check(result.classes == static_cast<int>(SrLayoutTable127004::kClassCount), "every class is counted");
        Check(result.checked == result.classes, "every class is checked");
        Check(result.mismatched == 0 && result.missing == 0 && result.deferred == 0, "0 mismatched, 0 missing, 0 deferred");
        Check(result.lines.empty(), "nothing to log");
        Check(!LayoutCheck::FoundChanges(result), "plugins stay on");
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
        Check(LayoutCheck::FoundChanges(result), "the change is found");
        Check(LayoutCheck::Decide(kTable126119(), result).safeOff, "the fixture has no consumer tags: plugins are switched off");
        Check(result.changes.size() == result.lines.size(), "one report entry per log line");

        const CompatReport::Inputs inputs{ "1.0.0.0-CL-127004", "StarRupture-Win64-Shipping.exe", std::string(64, 'a'), 126119,
            { { "Some_Pattern", "48 8B \"05\"", false, true } },
            { { "GObjects", true, 0x1000, 0x1000, false }, { "GWorld", false, 0, 0x2000, false },
              { "GNames", true, 0x3000, 0x4000, true } },
            &result, CompatReport::Outcome::SafeOff,
            { { "Plug", "FReflected", 127004, 44, true, 44, "match" },
              { "Plug", "FPlain", 127004, 24, false, 0, "row-only" },
              { "Plug", "FNoRow", 0, 0, false, 0, "no-row" },
              { "Plug", "FOnlyGame", 0, 0, true, 8, "reflected-only" } } };
        const std::string json = CompatReport::Build(inputs);
        const auto has = [&json](const char* text) { return json.find(text) != std::string::npos; };

        Check(has("\"productVersion\": \"1.0.0.0-CL-127004\""), "the game's ProductVersion");
        Check(has("\"exe\": { \"name\": \"StarRupture-Win64-Shipping.exe\", \"sha256\": \"aaaaaaaa"), "the exe's name and SHA-256");
        Check(has("\"sdkCL\": 126119"), "the SDK's CL");
        Check(has("\"outcome\": \"safe-off\""), "a safe-off says so");

        // A later safe-off writes the report again: the lines and changes of the
        // earlier partial switch-off, which the init thread carries over, stay.
        LayoutCheck::Result later = result;
        later.lines.push_back("GObjects or FName::AppendString was not found on this build");
        CompatReport::Inputs partial = inputs;
        partial.layout  = &later;
        partial.outcome = CompatReport::Outcome::Partial;
        const std::string partialJson = CompatReport::Build(partial);
        Check(partialJson.find("\"outcome\": \"partial\"") != std::string::npos
            && partialJson.find("\"outcome\": \"safe-off\"") == std::string::npos, "a partial switch-off says so");
        partial.outcome = CompatReport::Outcome::SafeOff;
        const std::string afterJson = CompatReport::Build(partial);
        Check(afterJson.find("\"outcome\": \"safe-off\"") != std::string::npos
            && afterJson.find("{ \"class\": \"ACrCharacterPlayerBase\", \"member\": \"ADSMoveSpeed\"") != std::string::npos
            && afterJson.find("\"mismatched\": 1") != std::string::npos,
            "a safe-off after a partial switch-off keeps the layout changes");
        Check(has("{ \"name\": \"Some_Pattern\", \"module\": \"StarRupture-Win64-Shipping.exe\", \"pattern\": \"48 8B \\\"05\\\"\", \"required\": false, \"found\": true }"),
            "a missed pattern, quotes escaped");
        Check(has("{ \"name\": \"GObjects\", \"resolved\": true, \"rva\": 4096, \"sdkOffset\": 4096, \"rejected\": false }")
            && has("{ \"name\": \"GWorld\", \"resolved\": false, \"rva\": 0, \"sdkOffset\": 8192, \"rejected\": false }"), "each global's RVA, resolved or not");
        Check(has("{ \"name\": \"GNames\", \"resolved\": true, \"rva\": 12288, \"sdkOffset\": 16384, \"rejected\": true }"),
            "a rejected hit keeps its RVA and is flagged");
        Check(has("{ \"plugin\": \"Plug\", \"key\": \"FReflected\", \"rowCL\": 127004, \"rowSize\": 44, \"reflectedSize\": 44, \"result\": \"match\" }")
            && has("{ \"plugin\": \"Plug\", \"key\": \"FPlain\", \"rowCL\": 127004, \"rowSize\": 24, \"reflectedSize\": null, \"result\": \"row-only\" }")
            && has("{ \"plugin\": \"Plug\", \"key\": \"FNoRow\", \"rowCL\": null, \"rowSize\": null, \"reflectedSize\": null, \"result\": \"no-row\" }")
            && has("{ \"plugin\": \"Plug\", \"key\": \"FOnlyGame\", \"rowCL\": null, \"rowSize\": null, \"reflectedSize\": 8, \"result\": \"reflected-only\" }"),
            "each size request: its row and the reflected size, null where there is none");
        Check(has("\"format\": 4") && has("\"sizes\": ["), "the report is format 4 and carries the sizes");

        // A boot with no layout change still writes the report when a plugin asked for a size.
        const LayoutCheck::Result quiet = CompareWith(kTable126119(), AsGame(kTable126119()));
        Check(!LayoutCheck::FoundChanges(quiet), "the fixture compared with itself finds no change");
        CompatReport::Inputs unchanged = inputs;
        unchanged.layout  = &quiet;
        unchanged.outcome = CompatReport::Outcome::Unchanged;
        const std::string unchangedJson = CompatReport::Build(unchanged);
        Check(unchangedJson.find("\"outcome\": \"unchanged\"") != std::string::npos
            && unchangedJson.find("\"mismatched\": 0") != std::string::npos
            && unchangedJson.find("{ \"plugin\": \"Plug\", \"key\": \"FPlain\"") != std::string::npos
            && unchangedJson.find("\"changes\": []") != std::string::npos,
            "an unchanged boot's report says so and still carries the sizes");
        Check(CompatReport::RefreshWrites(false, 1), "no layout change and one size request: the report is written");
        Check(!CompatReport::RefreshWrites(false, 0), "no layout change and no size request: no report");
        Check(CompatReport::RefreshWrites(true, 0), "a report already written this session is written again");
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

        const auto table = LayoutCheck::ReadTable(SrLayoutTable_130045::kNames, SrLayoutTable_130045::kClasses, SrLayoutTable_130045::kRows);
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
        Check(wholeResult.lines.empty() && !LayoutCheck::FoundChanges(wholeResult), "the whole member matches the game");
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

    // Dumper-7 renames the parameter Class to Class_0; the game reflects it as
    // Class. CL 127004, client: this one row switched every plugin off.
    void DumperRenamedMember()
    {
        std::printf("Dumper-7 renamed members\n");
        using LayoutCheck::Kind;

        const auto table = kLoaderTable();
        std::vector<LayoutCheck::TableClass> sdk;
        for (const LayoutCheck::TableClass& c : table)
        {
            if (std::string(c.name) == "SubsystemBlueprintLibrary_GetGameInstanceSubsystem")
                sdk.push_back(c);
        }
        Check(sdk.size() == 1 && sdk[0].rows.size() == 3 && std::string(sdk[0].rows[1].name) == "Class_0",
            "the loader's table has GetGameInstanceSubsystem's parameter as Class_0");
        if (sdk.size() != 1)
            return;

        // What the game reflected for Engine.SubsystemBlueprintLibrary.GetGameInstanceSubsystem.
        const auto game = [](int32_t classOffset, int32_t classSize)
        {
            return std::map<std::string, LayoutCheck::LiveStruct>{ { "Engine.SubsystemBlueprintLibrary.GetGameInstanceSubsystem",
                { 0x18, 1, { { "ContextObject", 0x0, 8, false, 0 }, { "Class", classOffset, classSize, false, 0 },
                             { "ReturnValue", 0x10, 8, false, 0 } } } } };
        };

        const LayoutCheck::Result same = CompareWith(sdk, game(0x8, 8));
        Check(same.lines.empty() && !LayoutCheck::FoundChanges(same), "Class_0 matches the game's Class: plugins stay on");

        const LayoutCheck::Result moved = CompareWith(sdk, game(0x10, 8));
        Check(Contains(moved, "SubsystemBlueprintLibrary_GetGameInstanceSubsystem::Class_0: not found in the game")
            && LayoutCheck::FoundChanges(moved) && LayoutCheck::Decide(sdk, moved).safeOff,
            "Class at another offset is no match: plugins are switched off (they compile the parameter block in)");

        const LayoutCheck::Result resized = CompareWith(sdk, game(0x8, 4));
        Check(Contains(resized, "SubsystemBlueprintLibrary_GetGameInstanceSubsystem::Class_0: not found in the game")
            && LayoutCheck::FoundChanges(resized) && LayoutCheck::Decide(sdk, resized).safeOff,
            "Class of another size is no match: plugins are switched off");

        // Only a _<digits> suffix is stripped, and never from a row of unknown size.
        const std::vector<LayoutCheck::TableClass> other = {
            { "FThing", "Mod.Thing", 0x10, 8, Kind::ScriptStruct, { { "Value_A", 0x0, 4, 0 }, { "Count_1", 0x4, 0, 0 } } },
        };
        const std::map<std::string, LayoutCheck::LiveStruct> thing = { { "Mod.Thing",
            { 0x10, 8, { { "Value", 0x0, 4, false, 0 }, { "Count", 0x4, 4, false, 0 } } } } };
        const LayoutCheck::Result result = CompareWith(other, thing);
        Check(Contains(result, "FThing::Value_A: not found in the game") && Contains(result, "FThing::Count_1: not found in the game")
            && result.lines.size() == 2, "Value_A and an unsized Count_1 stay missing");
    }

    void UsedMembersOnly()
    {
        std::printf("Granularity 1: the members the loader reads\n");
        using LayoutCheck::Kind;

        Check(SrLayoutTable_130045::kGranularity == 1, "the loader's table lists only the members it reads");
        const std::vector<LayoutCheck::TableClass> table = LayoutCheck::ReadTable(SrLayoutTable_130045::kNames, SrLayoutTable_130045::kClasses, SrLayoutTable_130045::kRows);
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
        Check(narrow.mismatched == 3 && narrow.lines.size() == 3 && LayoutCheck::FoundChanges(narrow), "nothing else");

        const LayoutCheck::Result whole = LayoutCheck::Compare(sdk, find);
        Check(Contains(whole, "UPerBuild::Resolved: offset SDK 0x30, game 0x48"), "without a per-build rule every row is compared");
    }

    // The loader's table is generated from the Client SDK (CL 130045). The
    // two members of its scope that sit elsewhere on CL 127004 are per-build
    // members, so a 127004 game is not a changed layout.
    void ClientTableOnEitherBuild()
    {
        std::printf("The loader's table on a 130045 and a 127004 game\n");

        Check(LayoutCheck::IsPluginPerBuildMember("UCrEnviroWaveTimerSubsystem", "TimerActor"),
            "UCrEnviroWaveTimerSubsystem::TimerActor is a per-build member");
        Check(!LayoutCheck::IsPluginPerBuildMember("UCrEnviroWaveTimerSubsystem", "Other")
            && !LayoutCheck::IsPluginPerBuildMember("ACrCrafter", "TimerActor"),
            "only that class and member");

        Check(SrLayoutTable_130045::kBuiltCL == 130045, "the table is the 130045 one");
        const std::vector<LayoutCheck::TableClass> table = kLoaderTable();

        const LayoutCheck::Result same = CompareWith(table, AsGame(table));
        Check(same.lines.empty() && !LayoutCheck::FoundChanges(same) && !LayoutCheck::Decide(table, same).safeOff,
            "a 130045 game: nothing changed, plugins stay on");

        auto game = AsGame(table);
        int moved = 0;
        for (const LayoutCheck::TableClass& c : table)
        {
            const std::string name = c.name;
            if (name != "UCrEnviroWaveTimerSubsystem" && name != "ACrCrafter")
                continue;
            for (LayoutCheck::LiveProperty& property : game[c.path].properties)
            {
                if (name == "UCrEnviroWaveTimerSubsystem" && property.name == "TimerActor" && property.offset == 0x98)
                {
                    property.offset = 0x90;
                    ++moved;
                }
                if (name == "ACrCrafter" && property.name == "CraftComponent" && property.offset == 0x6E0)
                {
                    property.offset = 0x6D8;
                    ++moved;
                }
            }
        }
        Check(moved == 2, "the table has TimerActor at 0x98 and CraftComponent at 0x6E0");

        const LayoutCheck::Result whole = CompareWith(table, game);
        Check(whole.mismatched == 2 && LayoutCheck::FoundChanges(whole),
            "a 127004 game with no per-build rule: the two members count as changes");

        const LayoutCheck::Result narrow = LayoutCheck::Compare(table,
            [&game](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
            {
                const auto it = game.find(c.path);
                return it == game.end() ? nullptr : &it->second;
            },
            [](const LayoutCheck::TableClass& c, const LayoutCheck::TableRow& row)
            {
                // IsPerBuildField also holds CraftComponent (SdkGlobals, needs the SDK).
                return LayoutCheck::IsPluginPerBuildMember(c.name, row.name)
                    || (std::string(c.name) == "ACrCrafter" && std::string(row.name) == "CraftComponent");
            });
        Check(narrow.perBuild == 2 && narrow.mismatched == 0 && narrow.lines.empty() && !LayoutCheck::FoundChanges(narrow)
            && !LayoutCheck::Decide(table, narrow).safeOff,
            "a 127004 game: both are left to the version map, no changed class, no safe-off");
    }

    // The per-build rule of the real check (SdkGlobals::IsPerBuildField needs the
    // SDK, so it is written out here): the plugin members, and CraftComponent.
    bool PerBuildRule(const LayoutCheck::TableClass& c, const LayoutCheck::TableRow& row)
    {
        return LayoutCheck::IsPluginPerBuildMember(c.name, row.name)
            || (std::string(c.name) == "ACrCrafter" && std::string(row.name) == "CraftComponent");
    }

    // The new table checked against the layout a 127004 game really has. The
    // 127004 side is the loader's own table of v1.26.1, which was made from the
    // 127004 SDK, so this is the real 127004 layout of every class that table
    // lists. The current table lists more classes and members than v1.26.1's
    // did, so the comparison is of the classes and members both have.
    void NewTableOnARealLayoutOf127004()
    {
        std::printf("The 130045 table on the 127004 layout\n");
        const std::vector<LayoutCheck::TableClass> old = kLoaderTable127004();
        const std::vector<LayoutCheck::TableClass> table = OnlyWhatIn(kLoaderTable(), old);
        Check(SrLayoutTable127004Loader::kBuiltCL == 127004 && SrLayoutTable_130045::kBuiltCL == 130045, "the two tables are the 127004 and 130045 ones");
        Check(table.size() == old.size(), "the table lists every class v1.26.1's table did");

        const auto game = AsGame(old);
        const auto find = [&game](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
        {
            const auto it = game.find(c.path);
            return it == game.end() ? nullptr : &it->second;
        };

        const LayoutCheck::Result bare = LayoutCheck::Compare(table, find);
        Check(bare.mismatched == 2 && bare.checked == bare.classes && bare.missing == 0,
            "without a per-build rule exactly two members differ and every class is found");
        Check(Contains(bare, "UCrEnviroWaveTimerSubsystem::TimerActor: offset SDK 0x98, game 0x90")
            && Contains(bare, "ACrCrafter::CraftComponent: offset SDK 0x6E0, game 0x6D8"),
            "they are TimerActor and CraftComponent");

        const LayoutCheck::Result result = LayoutCheck::Compare(table, find, PerBuildRule);
        const LayoutCheck::Verdict verdict = LayoutCheck::Decide(table, result);
        Check(result.perBuild == 2 && result.mismatched == 0 && result.lines.empty() && result.changedClasses.empty(),
            "with the per-build rule: no changed class");
        Check(!LayoutCheck::FoundChanges(result) && !verdict.safeOff && verdict.featuresOff.empty() && verdict.changed.empty(),
            "no safe-off, no feature off, no plugin refused for a changed class");
    }

    // The same two layouts the other way round: what v1.26.1 (127004 table, CraftComponent
    // alone per build) made of a 130045 game. TimerActor was a changed class there.
    void OldLoaderOn130045()
    {
        std::printf("The v1.26.1 loader on the 130045 layout\n");
        const std::vector<LayoutCheck::TableClass> old = kLoaderTable127004();
        const auto game = AsGame(kLoaderTable());
        const auto find = [&game](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
        {
            const auto it = game.find(c.path);
            return it == game.end() ? nullptr : &it->second;
        };
        const LayoutCheck::Result result = LayoutCheck::Compare(old, find,
            [](const LayoutCheck::TableClass& c, const LayoutCheck::TableRow& row)
            {
                return std::string(c.name) == "ACrCrafter" && std::string(row.name) == "CraftComponent";
            });
        const LayoutCheck::Verdict verdict = LayoutCheck::Decide(old, result);
        Check(result.mismatched == 1 && Contains(result, "UCrEnviroWaveTimerSubsystem::TimerActor: offset SDK 0x90, game 0x98"),
            "v1.26.1 on 130045: TimerActor is the one difference");
        Check(verdict.changed == std::vector<std::string>{ "UCrEnviroWaveTimerSubsystem" },
            "so UCrEnviroWaveTimerSubsystem was a changed class");
        std::printf("  info: that verdict: safeOff=%d, featuresOff=%zu\n", verdict.safeOff ? 1 : 0, verdict.featuresOff.size());
    }
}

namespace
{
    // What a change switches off, by what reads the changed class.
    void ConsumersDecideWhatStops()
    {
        std::printf("Consumers: what a change switches off\n");
        using LayoutCheck::Kind;

        // The loader's table carries the tags, and they say what they should.
        const auto table = kLoaderTable();
        const auto tags = [&table](const char* name)
        {
            const LayoutCheck::TableClass* c = ClassNamed(table, name);
            return c ? c->consumers : std::vector<std::string>{ "(not in the table)" };
        };
        const auto has = [](const std::vector<std::string>& ids, const char* id)
        {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        Check(tags("UCrUW_MapMenu") == std::vector<std::string>{ "ui-menus" }, "UCrUW_MapMenu: only the menu-layer read");
        Check(tags("ACrCharacterPlayerBase") == std::vector<std::string>{ "keys" }, "ACrCharacterPlayerBase: only a version-map key");
        Check(has(tags("UWorld"), "core"), "UWorld: the loader's own code");
        Check(has(tags("SubsystemBlueprintLibrary_GetGameInstanceSubsystem"), "plugin-sdk"), "a parameter block: compiled into plugins");
        Check(has(tags("UCommonActivatableWidgetContainerBase"), "ui-menus"), "WidgetList's class: the menu-layer read");
        Check(has(tags("UPrimaryGameLayout"), "ui-menus"), "Layers' class: the menu-layer read");
        bool allTagged = true;
        for (const LayoutCheck::TableClass& c : table)
            allTagged = allTagged && !c.consumers.empty();
        Check(allTagged, "every class in the loader's table has a consumer");

        // The real table on a game where a menu screen class is gone: only
        // the menu-layer read stops.
        {
            auto game = AsGame(table);
            game.erase(ClassNamed(table, "UCrUW_MapMenu")->path);
            const LayoutCheck::Result result = CompareWith(table, game);
            const LayoutCheck::Verdict verdict = LayoutCheck::Decide(table, result);
            Check(LayoutCheck::FoundChanges(result) && !verdict.safeOff
                && verdict.featuresOff == std::vector<std::string>{ "ui-menus" }
                && verdict.changed == std::vector<std::string>{ "UCrUW_MapMenu" },
                "a class only the menu-layer read uses: that feature off, plugins stay on");
        }
        // The menu read takes the layer map from UPrimaryGameLayout: a move
        // of Layers there is found, and the read is off (here with every
        // plugin, because plugins compile that class in too).
        {
            auto game = AsGame(table);
            for (LayoutCheck::LiveProperty& property : game[ClassNamed(table, "UPrimaryGameLayout")->path].properties)
                if (property.name == "Layers")
                    property.offset += 8;
            const LayoutCheck::Result result = CompareWith(table, game);
            const LayoutCheck::Verdict verdict = LayoutCheck::Decide(table, result);
            Check(LayoutCheck::FoundChanges(result) && verdict.changed == std::vector<std::string>{ "UPrimaryGameLayout" }
                && has(verdict.featuresOff, "ui-menus") && verdict.safeOff,
                "UPrimaryGameLayout::Layers moved: found, the menu read is off");
        }
        // ...and where the player class lost its members: nothing in the
        // loader stops; plugins that keep it compiled in are refused
        // (VersionMapTests).
        {
            auto game = AsGame(table);
            game[ClassNamed(table, "ACrCharacterPlayerBase")->path].properties.clear();
            const LayoutCheck::Result result = CompareWith(table, game);
            const LayoutCheck::Verdict verdict = LayoutCheck::Decide(table, result);
            Check(LayoutCheck::FoundChanges(result) && !verdict.safeOff && verdict.featuresOff.empty()
                && verdict.changed == std::vector<std::string>{ "ACrCharacterPlayerBase" }
                && verdict.changedPaths == std::vector<std::string>{ "Chimera.CrCharacterPlayerBase" },
                "a class plugins read through the map: listed as changed, no loader feature off, no safe-off");
        }

        // Synthetic tags, one changed class each.
        const auto decide = [](std::vector<std::string> consumers)
        {
            const std::vector<LayoutCheck::TableClass> sdk = {
                { "FThing", "Mod.Thing", 0x10, 8, Kind::ScriptStruct, { { "A", 0x0, 8, 0 } }, std::move(consumers) },
            };
            const std::map<std::string, LayoutCheck::LiveStruct> game = { { "Mod.Thing", { 0x10, 8, { { "A", 0x8, 8, false, 0 } } } } };
            return LayoutCheck::Decide(sdk, CompareWith(sdk, game));
        };
        const LayoutCheck::Verdict menus = decide({ "ui-menus" });
        Check(!menus.safeOff && menus.featuresOff == std::vector<std::string>{ "ui-menus" }, "ui-menus: that feature off");
        const LayoutCheck::Verdict keys = decide({ "keys" });
        Check(!keys.safeOff && keys.featuresOff.empty() && keys.changed == std::vector<std::string>{ "FThing" },
            "keys: nothing in the loader, listed as changed");
        Check(decide({}).safeOff, "no consumer: safe-off");
        Check(decide({ "core" }).safeOff, "core: safe-off");
        Check(decide({ "plugin-sdk" }).safeOff, "plugin-sdk: safe-off");
        Check(decide({ "future-feature" }).safeOff && decide({ "?" }).safeOff, "an id this loader does not know: safe-off");
        Check(decide({ "core", "ui-menus" }).safeOff && decide({ "keys", "plugin-sdk" }).safeOff,
            "a tag that needs safe-off wins over the others");

        // A changed class the table does not list, and a difference with no class.
        LayoutCheck::Result stray;
        stray.mismatched = 1;
        stray.changedClasses = { "AStray" };
        stray.lines = { "AStray: size SDK 0x10, game 0x18" };
        Check(LayoutCheck::Decide({}, stray).safeOff, "a changed class not in the table: safe-off");
        LayoutCheck::Result unnamed;
        unnamed.lines = { "something differs" };
        Check(LayoutCheck::Decide({}, unnamed).safeOff, "a difference that names no class: safe-off");
        const LayoutCheck::Verdict none = LayoutCheck::Decide(table, CompareWith(table, AsGame(table)));
        Check(!none.safeOff && none.featuresOff.empty() && none.changed.empty(), "no change: nothing off");

        // A bit with no id in kConsumers reads as "?".
        struct Cls { uint32_t name, sizeOf, alignOf, firstRow, rowCount, kind, path; };
        struct Row { uint32_t name, offset, size, boolMask; };
        const char* const names[2] = { "FThing", "Mod.Thing" };
        const Cls classes[1] = { { 0, 0x10, 8, 0, 0, 2, 1 } };
        const Row rows[1] = { { 0, 0, 0, 0 } };
        const char* const consumerIds[1] = { "ui-menus" };
        const uint32_t classConsumers[1] = { 0x3 };
        const auto read = LayoutCheck::ReadTable(names, classes, rows, consumerIds, classConsumers);
        Check(read.size() == 1 && read[0].consumers == std::vector<std::string>{ "ui-menus", "?" },
            "a bit past kConsumers reads as an unknown id");
    }

    // The UObject vtable stands in as an array: slot `index` holds the address
    // a game build puts ProcessEvent at, the others hold other functions.
    void ProcessEventSlotDecidesSafeOff()
    {
        std::printf("UObject::ProcessEvent vtable slot\n");
        constexpr int32_t index = 0x4C;
        constexpr uintptr_t base = 0x140000000;
        constexpr uintptr_t resolved = base + 0x177FB20;
        uintptr_t vtable[0x60] = {};
        for (size_t i = 0; i < std::size(vtable); ++i)
            vtable[i] = base + 0x100000 + i * 0x10;

        vtable[index] = resolved;
        LayoutCheck::Result match;
        Check(LayoutCheck::CheckProcessEventSlot(match, vtable[index], index, resolved, base), "the slot holds the map's function: it passes");
        Check(match.lines.empty() && match.changes.empty() && match.slotMismatched == 0 && !LayoutCheck::FoundChanges(match),
            "nothing to log, plugins stay on");
        Check(!LayoutCheck::Decide(kLoaderTable(), match).safeOff, "no safe-off verdict");

        // A UObject with a virtual added ahead of ProcessEvent: the function
        // moved one slot down, and the compiled index reads another function.
        vtable[index] = base + 0x100000 + index * 0x10;
        vtable[index + 1] = resolved;
        LayoutCheck::Result moved;
        Check(!LayoutCheck::CheckProcessEventSlot(moved, vtable[index], index, resolved, base), "the slot holds another function: it fails");
        Check(moved.slotMismatched == 1 && LayoutCheck::FoundChanges(moved), "the difference is found");
        Check(moved.lines == std::vector<std::string>{ "UObject::ProcessEvent: vtable slot 0x4C holds base+0x1004C0, the function is at base+0x177FB20" },
            "one line, with both offsets in the image");
        Check(moved.changes.size() == 1 && moved.changes[0].className == "UObject" && moved.changes[0].member == "ProcessEvent"
                && moved.changes[0].field == LayoutCheck::kChangeVtableSlot && moved.changes[0].sdk == 0x177FB20
                && moved.changes[0].game == 0x1004C0,
            "the report entry names the slot and both offsets");
        Check(moved.changedClasses.empty() && moved.mismatched == 0 && moved.missing == 0,
            "no class is counted as changed");
        const LayoutCheck::Verdict verdict = LayoutCheck::Decide(kLoaderTable(), moved);
        Check(verdict.safeOff && verdict.featuresOff.empty(), "safe-off: every plugin off");

        // A class changed elsewhere (here one only the menu layer reads) does
        // not hide the slot: the verdict is still safe-off.
        {
            auto table = kLoaderTable();
            const std::string menu = ClassNamed(table, "UCrUW_MapMenu")->path;
            auto game = AsGame(table);
            game.erase(menu);
            LayoutCheck::Result both = CompareWith(table, game);
            Check(!LayoutCheck::Decide(table, both).safeOff, "the menu class alone: plugins stay on");
            LayoutCheck::CheckProcessEventSlot(both, vtable[index], index, resolved, base);
            Check(both.slotMismatched == 1 && !both.changedClasses.empty(), "a changed class and a slot mismatch together");
            const LayoutCheck::Verdict together = LayoutCheck::Decide(table, both);
            Check(together.safeOff && together.featuresOff == std::vector<std::string>{ "ui-menus" },
                "the slot mismatch forces safe-off; the class still switches its feature off");
        }

        // It is the slot that is read, not the whole table.
        vtable[index] = resolved;
        vtable[index + 1] = base;
        LayoutCheck::Result neighbour;
        Check(LayoutCheck::CheckProcessEventSlot(neighbour, vtable[index], index, resolved, base) && neighbour.lines.empty(),
            "a different function in the next slot does not matter");

        // A vtable the slot read faults on (here a page nothing may touch)
        // is "cannot verify": no slot, no difference, and no crash.
        {
            uintptr_t read = 0x1234;
            uint32_t code = 0;
            Check(LayoutCheck::ReadVtableSlot(vtable, index, read, code) && read == resolved && code == 0,
                "a readable vtable: the slot comes back");

            void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
            Check(page != nullptr, "a page to fault on");
            if (page)
            {
                read = 0x1234;
                Check(!LayoutCheck::ReadVtableSlot(static_cast<const uintptr_t*>(page), index, read, code)
                        && code == EXCEPTION_ACCESS_VIOLATION && read == 0x1234,
                    "an unreadable vtable: the read fails with the exception code, the slot is left alone");
                VirtualFree(page, 0, MEM_RELEASE);
            }
            Check(!LayoutCheck::ReadVtableSlot(nullptr, index, read, code) && code == EXCEPTION_ACCESS_VIOLATION,
                "a null vtable: the read fails the same way");
        }
    }

    // ---- Function parameter structs: the live offsets and ParmsSize ----

    // A function as the game would reflect it, from the table's parameter
    // struct: every parameter at or after `shiftFrom` moves by `shiftBy`, as if
    // an update had put a parameter of that size in front of it. A name Dumper-7
    // suffixed (Class_0) is reflected without the suffix.
    LayoutCheck::LiveFunction GameFunction(const std::vector<LayoutCheck::TableClass>& table, const char* path,
        int32_t shiftFrom = INT32_MAX, int32_t shiftBy = 0)
    {
        LayoutCheck::LiveFunction function;
        for (const LayoutCheck::TableClass& c : table)
        {
            if (c.kind != LayoutCheck::Kind::FunctionParams || std::string(c.path) != path)
                continue;
            function.path = c.path;
            for (const LayoutCheck::TableRow& row : c.rows)
            {
                std::string name = row.name;
                if (name == "Class_0")
                    name = "Class";
                const int32_t offset = static_cast<int32_t>(row.offset);
                function.params.push_back(LayoutCheck::LiveProperty{ name, offset >= shiftFrom ? offset + shiftBy : offset,
                    static_cast<int32_t>(row.size), false, 0 });
            }
        }
        return function;
    }

    const LayoutCheck::TableClass* TableFunction(const std::vector<LayoutCheck::TableClass>& table, const char* path)
    {
        for (const LayoutCheck::TableClass& c : table)
            if (c.kind == LayoutCheck::Kind::FunctionParams && std::string(c.path) == path)
                return &c;
        return nullptr;
    }

    void FunctionParamsFollowTheGame()
    {
        std::printf("function parameter structs\n");
        const auto table = LayoutCheck::ReadTable(SrLayoutTable_130045::kNames, SrLayoutTable_130045::kClasses, SrLayoutTable_130045::kRows);
        const char* const kPlayer = "Engine.GameplayStatics.GetPlayerController";
        const char* const kPlayerName = "GameplayStatics_GetPlayerController";

        // The same build as the SDK: every parameter of every function the
        // table lists is where the table has it, and ParmsSize is the end of
        // the last parameter (the table's sizeof rounded up to the struct's
        // alignment).
        bool allSame = true;
        bool sizesAgree = true;
        int functions = 0;
        for (const LayoutCheck::TableClass& c : table)
        {
            if (c.kind != LayoutCheck::Kind::FunctionParams)
                continue;
            ++functions;
            const std::vector<LayoutCheck::LiveFunction> game{ GameFunction(table, c.path) };
            for (const LayoutCheck::TableRow& row : c.rows)
            {
                const LayoutCheck::ParamMember member = LayoutCheck::ResolveParamMember(c.name, game, row.name);
                allSame = allSame && member.error.empty() && member.offset == static_cast<int32_t>(row.offset)
                    && member.size == row.size;
            }
            const LayoutCheck::ParamMember size = LayoutCheck::ResolveParamMember(c.name, game, "");
            sizesAgree = sizesAgree && size.error.empty() && size.size <= c.sizeOf && c.sizeOf - size.size < 16;
        }
        Check(functions >= 8, "the table lists function parameter structs to read");
        Check(allSame, "a parameter resolves to its live offset and size, for every function in the table");
        Check(sizesAgree, "the block size is the end of the last parameter, within the struct's padding");

        {
            const std::vector<LayoutCheck::LiveFunction> game{ GameFunction(table, kPlayer) };
            Check(LayoutCheck::ResolveParamMember(kPlayerName, game, "ReturnValue").offset == 16,
                "GetPlayerController: ReturnValue at 0x10");
            Check(LayoutCheck::ResolveParamMember(kPlayerName, game, "").size == 0x18,
                "GetPlayerController: ParmsSize 0x18");
            Check(LayoutCheck::ResolveParamMember(kPlayerName, game, "returnvalue").offset == 16,
                "a name differing only in case still resolves");
            // Not rounded up: the last parameter is a one-byte bool at 8.
            const std::vector<LayoutCheck::LiveFunction> only{ GameFunction(table, "UMG.WidgetBlueprintLibrary.SetInputMode_GameOnly") };
            Check(LayoutCheck::ResolveParamMember("WidgetBlueprintLibrary_SetInputMode_GameOnly", only, "").size == 9,
                "ParmsSize is the end of the last parameter, not rounded up to the struct's alignment");
        }

        // An update puts an 8-byte parameter before PlayerIndex: PlayerIndex
        // and ReturnValue move and ParmsSize grows. The compiled offsets are
        // the table's.
        {
            const LayoutCheck::TableClass* compiled = TableFunction(table, kPlayer);
            const std::vector<LayoutCheck::LiveFunction> game{ GameFunction(table, kPlayer, 8, 8) };
            const LayoutCheck::ParamMember playerIndex = LayoutCheck::ResolveParamMember(kPlayerName, game, "PlayerIndex");
            const LayoutCheck::ParamMember ret = LayoutCheck::ResolveParamMember(kPlayerName, game, "ReturnValue");
            const LayoutCheck::ParamMember size = LayoutCheck::ResolveParamMember(kPlayerName, game, "");
            Check(compiled && compiled->rows.size() == 3 && compiled->rows[2].offset == 16, "the compiled ReturnValue is at 0x10");
            Check(ret.error.empty() && ret.offset == 24 && ret.size == 8, "a shifted ReturnValue is found at its live offset");
            Check(playerIndex.offset == 16 && playerIndex.size == 4, "so is a shifted in-parameter");
            Check(size.size == 0x20, "and the block size follows the added parameter");
            const LayoutCheck::ParamMember world = LayoutCheck::ResolveParamMember(kPlayerName, game, "WorldContextObject");
            Check(world.offset == 0, "a parameter before the insertion stays put");
        }

        // The Dumper-7 suffix on a reserved name.
        {
            const char* const name = "SubsystemBlueprintLibrary_GetGameInstanceSubsystem";
            const std::vector<LayoutCheck::LiveFunction> game{ GameFunction(table, "Engine.SubsystemBlueprintLibrary.GetGameInstanceSubsystem") };
            const LayoutCheck::ParamMember cls = LayoutCheck::ResolveParamMember(name, game, "Class_0");
            Check(cls.error.empty() && cls.offset == 8 && cls.size == 8, "Class_0 is found as the game's Class");
            Check(LayoutCheck::ResolveParamMember(name, game, "Class").offset == 8, "and by the game's own name");
        }

        // Refusals.
        {
            const std::vector<LayoutCheck::LiveFunction> game{ GameFunction(table, kPlayer) };
            LayoutCheck::ParamMember r = LayoutCheck::ResolveParamMember(kPlayerName, game, "NoSuchParam");
            Check(!r.error.empty() && r.error.find("NoSuchParam") != std::string::npos && r.error.find(kPlayer) != std::string::npos,
                "an unknown parameter is refused, naming it and the function");
            r = LayoutCheck::ResolveParamMember("GameplayStatics_GetNothing", {}, "ReturnValue");
            Check(!r.error.empty() && r.error.find("GameplayStatics_GetNothing") != std::string::npos,
                "a name no function carries is refused");
            r = LayoutCheck::ResolveParamMember("GameplayStatics_GetNothing", {}, "");
            Check(!r.error.empty(), "so is its block size");

            // The same class name in two modules carries the same parameter struct name.
            std::vector<LayoutCheck::LiveFunction> two{ GameFunction(table, kPlayer), GameFunction(table, kPlayer) };
            two[1].path = "Other.GameplayStatics.GetPlayerController";
            r = LayoutCheck::ResolveParamMember(kPlayerName, two, "ReturnValue");
            Check(!r.error.empty() && r.error.find(kPlayer) != std::string::npos
                    && r.error.find("Other.GameplayStatics.GetPlayerController") != std::string::npos
                    && r.error.find("/Script/") != std::string::npos,
                "a name two functions carry is refused, naming both and how to pick one");
            r = LayoutCheck::ResolveParamMember(kPlayerName, two, "");
            Check(!r.error.empty(), "so is its block size");
        }

        // How a name is read as <Class>_<Function>.
        {
            const auto plain = LayoutCheck::SplitParamStruct("CrEnviroWaveSubsystem_GetCurrentStageSettings");
            Check(plain.size() == 1 && plain[0].owner == "CrEnviroWaveSubsystem" && plain[0].function == "GetCurrentStageSettings"
                    && plain[0].module.empty(),
                "a name splits at its underscore");
            const auto two = LayoutCheck::SplitParamStruct("WidgetBlueprintLibrary_SetInputMode_GameAndUIEx");
            Check(two.size() == 2 && two[0].owner == "WidgetBlueprintLibrary" && two[0].function == "SetInputMode_GameAndUIEx"
                    && two[1].owner == "WidgetBlueprintLibrary_SetInputMode" && two[1].function == "GameAndUIEx",
                "an underscore in the function splits two ways, for the game to decide");
            const auto scripted = LayoutCheck::SplitParamStruct("/Script/UMG.WidgetBlueprintLibrary_Create");
            Check(scripted.size() == 1 && scripted[0].module == "UMG" && scripted[0].owner == "WidgetBlueprintLibrary"
                    && scripted[0].function == "Create",
                "/Script/<Module>. names the module");
            Check(LayoutCheck::SplitParamStruct("ACrCharacterPlayerBase").empty() && LayoutCheck::SplitParamStruct("_X").empty()
                    && LayoutCheck::SplitParamStruct("X_").empty() && LayoutCheck::SplitParamStruct("/Script/UMG").empty(),
                "a class name, or an empty half, does not split");
        }
    }
}

namespace
{
    // ---- Blueprint functions: pending until the class is loaded, then kept ----

    // WBP_InventorySlot_C::SetSlotSize as the game reflects it: InSize in, a
    // local the Blueprint function declares after it (not a parameter).
    std::vector<LayoutCheck::FunctionProperty> SlotSizeChildren(int32_t inSizeSize)
    {
        using namespace LayoutCheck;
        return {
            FunctionProperty{ LiveProperty{ "InSize", 0, inSizeSize, false, 0 }, kPropertyParm | 0x1 },
            FunctionProperty{ LiveProperty{ "CallFunc_Add_ReturnValue", 0x10, 8, false, 0 }, 0x0 },
            FunctionProperty{ LiveProperty{ "Temp_bool_Variable", 0x18, 1, false, 0 }, 0x0 },
        };
    }

    void BlueprintFunctionsPendThenResolve()
    {
        using namespace LayoutCheck;
        using Clock = BlueprintParams::Clock;
        std::printf("blueprint function parameters\n");

        const std::string name = "WBP_InventorySlot_C_SetSlotSize";
        const Clock::time_point t0{};
        const auto sec = [](int n) { return std::chrono::seconds(n); };

        {
            const auto splits = BlueprintSplits(name);
            Check(splits.size() == 1 && splits[0].owner == "WBP_InventorySlot_C" && splits[0].function == "SetSlotSize",
                "a Blueprint class (_C) is one split of the name");
            Check(BlueprintSplits("CrEnviroWaveSubsystem_GetCurrentStageSettings").empty()
                    && BlueprintSplits("/Script/UMG.WBP_X_C_Run").empty() && BlueprintSplits("WBP_Slot_SetSize").empty(),
                "a native name, a /Script/ name and a name with no _C class are not Blueprint names");
        }

        // The locals of the Blueprint function are not parameters: they do not
        // count and they do not stretch the block.
        {
            const LiveFunction fn = FunctionOf("Game.WBP_InventorySlot_C.SetSlotSize", SlotSizeChildren(8));
            Check(fn.params.size() == 1 && fn.params[0].name == "InSize" && ParmsSizeOf(fn) == 8,
                "locals are left out: one parameter, a block of 8 bytes");
        }

        int calls = 0;
        bool loaded = false;
        int32_t inSizeSize = 8;
        const BlueprintFinder find = [&](const std::vector<ParamSplit>&)
        {
            ++calls;
            BlueprintFind found;
            found.classLoaded = loaded;
            if (loaded)
                found.functions.push_back(FunctionOf("Game.WBP_InventorySlot_C.SetSlotSize", SlotSizeChildren(inSizeSize)));
            return found;
        };

        BlueprintParams cache(sec(2));
        bool pending = false;

        // Not loaded: pending, not an error that stops anything, and not a "no native" answer.
        ParamMember r = cache.Resolve(name, "", t0, true, find, pending);
        Check(pending && !r.error.empty() && r.error.rfind("no native", 0) != 0 && calls == 1,
            "not loaded: pending, with one look");
        r = cache.Resolve(name, "InSize", t0 + sec(1), true, find, pending);
        Check(pending && calls == 1, "retried inside the interval: pending, and no look at the game");
        for (int i = 0; i < 100; ++i)
            cache.Resolve(name, "InSize", t0 + std::chrono::milliseconds(1000 + i), true, find, pending);
        Check(calls == 1, "a hundred more retries inside the interval look at nothing");
        r = cache.Resolve(name, "InSize", t0 + sec(2), true, find, pending);
        Check(pending && calls == 2, "after the interval it looks again");
        r = cache.Resolve(name, "InSize", t0 + sec(60), false, find, pending);
        Check(pending && calls == 2 && r.error.find("game thread") != std::string::npos,
            "off the game thread it does not look, and says so");
        r = cache.Resolve(name, "InSize", t0 + sec(61), true, find, pending);
        Check(pending && calls == 3, "an off-thread request did not use up the interval");

        // The class loads: the numbers, once.
        loaded = true;
        r = cache.Resolve(name, "", t0 + sec(70), true, find, pending);
        Check(!pending && r.error.empty() && r.size == 8 && calls == 4, "loaded: the block size is the parameter's end, locals left out");
        r = cache.Resolve(name, "InSize", t0 + sec(70), true, find, pending);
        Check(!pending && r.error.empty() && r.offset == 0 && r.size == 8 && calls == 4,
            "loaded: the parameter, answered from what was seen");

        // The class is freed again (main menu), and the game reloads it with other numbers: the process keeps the first.
        loaded = false;
        inSizeSize = 16;
        r = cache.Resolve(name, "", t0 + sec(200), true, find, pending);
        Check(!pending && r.size == 8 && calls == 4, "unloaded again: the same numbers, and no look at the game");
        r = cache.Resolve(name, "InSize", t0 + sec(200), false, find, pending);
        Check(!pending && r.size == 8 && calls == 4, "unloaded and off the game thread: still the same numbers");

        // A pending name does not hold up another.
        r = cache.Resolve("WBP_Other_C_Run", "", t0 + sec(70), true, find, pending);
        Check(pending && calls == 5, "another name is looked up on its own");

        // The class is loaded and the request is wrong: a final answer, found once.
        {
            int looks = 0;
            const BlueprintFinder wrong = [&](const std::vector<ParamSplit>&)
            {
                ++looks;
                BlueprintFind found;
                found.classLoaded = true;
                found.functions.push_back(FunctionOf("Game.WBP_Wrong_C.Run", SlotSizeChildren(8)));
                return found;
            };
            r = cache.Resolve("WBP_Wrong_C_Run", "Size", t0, true, wrong, pending);
            Check(!pending && r.error.find("no parameter Size") != std::string::npos && looks == 1,
                "a parameter the loaded function does not have is refused, not pending");
            r = cache.Resolve("WBP_Wrong_C_Run", "Size", t0 + sec(100), true, wrong, pending);
            Check(!pending && !r.error.empty() && looks == 1, "and the refusal is final: no new look");

            const BlueprintFinder noFunction = [&](const std::vector<ParamSplit>&)
            {
                ++looks;
                BlueprintFind found;
                found.classLoaded = true;
                return found;
            };
            r = cache.Resolve("WBP_Wrong_C_Walk", "", t0, true, noFunction, pending);
            Check(!pending && r.error.find("not the parameter struct of any function") != std::string::npos,
                "a loaded class without the function is refused, not pending");

            // Two copies of the class (the old one not collected yet) with the same parameters are one function.
            const BlueprintFinder twice = [&](const std::vector<ParamSplit>&)
            {
                BlueprintFind found;
                found.classLoaded = true;
                found.functions.push_back(FunctionOf("Game.WBP_Twice_C.Run", SlotSizeChildren(8)));
                found.functions.push_back(FunctionOf("Game.WBP_Twice_C.Run", SlotSizeChildren(8)));
                return found;
            };
            r = cache.Resolve("WBP_Twice_C_Run", "InSize", t0, true, twice, pending);
            Check(!pending && r.error.empty() && r.size == 8, "two copies of one class are one function");

            // Two classes of the name with other parameters are ambiguous.
            const BlueprintFinder differ = [&](const std::vector<ParamSplit>&)
            {
                BlueprintFind found;
                found.classLoaded = true;
                found.functions.push_back(FunctionOf("Game.WBP_Differ_C.Run", SlotSizeChildren(8)));
                found.functions.push_back(FunctionOf("Game.WBP_Differ_C.Run", SlotSizeChildren(16)));
                return found;
            };
            r = cache.Resolve("WBP_Differ_C_Run", "", t0, true, differ, pending);
            Check(!pending && r.error.find("2 functions") != std::string::npos, "two functions that differ are ambiguous");
        }

        // The result owns its numbers: nothing of the finder's answer is needed afterwards.
        {
            BlueprintParams fresh(sec(2));
            std::vector<LiveFunction> source;
            const BlueprintFinder once = [&](const std::vector<ParamSplit>&)
            {
                BlueprintFind found;
                found.classLoaded = true;
                found.functions = source;
                return found;
            };
            source.push_back(FunctionOf("Game.WBP_Own_C.Run", SlotSizeChildren(8)));
            fresh.Resolve("WBP_Own_C_Run", "", t0, true, once, pending);
            source.clear();
            source.shrink_to_fit();
            r = fresh.Resolve("WBP_Own_C_Run", "InSize", t0 + sec(5), true, nullptr, pending);
            Check(!pending && r.size == 8, "the answer outlives the data it was copied from, and needs no finder once seen");
        }
    }
}

namespace
{
    // ---- A type reflection did not find: a plain C++ type, or a request that missed ----

    void PlainTypesAreToldFromMisses()
    {
        std::printf("Plain C++ types and requests that missed\n");
        // FD3D12Resource: nothing in the game carries D3D12Resource.
        Check(LayoutCheck::IsPlainType('F', false, false), "an F name nothing carries is a plain type");
        // FTexture: the game has the class Texture (UTexture) and no struct of that name.
        Check(LayoutCheck::IsPlainType('F', false, true), "an F name only a class carries is a plain type (FTexture beside UTexture)");
        Check(!LayoutCheck::IsPlainType('F', true, false), "an F name a struct carries elsewhere is a request that missed");
        Check(!LayoutCheck::IsPlainType('F', true, true), "so is one a struct and a class carry");
        // A class not in this build (a subsystem a game update added) is not reflected here.
        Check(LayoutCheck::IsPlainType('U', false, false) && LayoutCheck::IsPlainType('A', false, false),
            "a U or A name nothing carries is not reflected on this build");
        Check(!LayoutCheck::IsPlainType('U', true, false) && !LayoutCheck::IsPlainType('A', true, false),
            "a U or A name only a struct carries is a struct asked for as a class");
        Check(!LayoutCheck::IsPlainType('U', false, true) && !LayoutCheck::IsPlainType('A', false, true),
            "a U or A name a class carries elsewhere is a wrong /Script path");
    }
}

namespace
{
    // ---- The engine-init wait and what the player is told when it runs out ----

    void EngineWaitDeadlineAndWording()
    {
        std::printf("The engine-init wait: a true deadline, and the wording when it ran out\n");

        // The deadline is computed once; a message that ends a wait early leaves
        // only the time that is really left, never a fresh full wait.
        const uint64_t start = 1'000'000;
        const uint64_t deadline = start + StartupWait::kEngineTimeoutMs;
        Check(StartupWait::RemainingMs(start, deadline) == StartupWait::kEngineTimeoutMs, "at the start the whole wait is left");
        Check(StartupWait::RemainingMs(start + 38'700, deadline) == StartupWait::kEngineTimeoutMs - 38'700,
            "a message 38.7 s in leaves 81.3 s, not another 120 s");
        Check(StartupWait::RemainingMs(deadline - 1, deadline) == 1, "one millisecond before the deadline: 1 ms");
        Check(StartupWait::RemainingMs(deadline, deadline) == 0, "at the deadline: nothing left");
        Check(StartupWait::RemainingMs(deadline + 5'000, deadline) == 0, "past the deadline: still 0, no wrap-around");
        Check(StartupWait::kEngineTimeoutMs == 120'000 && StartupWait::TimeoutSeconds() == 120, "the wait is 2 minutes");

        // Only an engine init that was never observed after a wait that ran out is "the game did not
        // finish starting". The rest keeps the mismatch wording.
        using StartupWait::ClassifySafeOff;
        using StartupWait::SafeOffKind;
        Check(ClassifySafeOff(false, true) == SafeOffKind::CheckNotRun, "never observed, the wait ran out: the game did not finish starting");
        Check(ClassifySafeOff(false, false) == SafeOffKind::Mismatch, "never observed, no wait (hook failed to install): mismatch wording stays");
        Check(ClassifySafeOff(true, false) == SafeOffKind::Mismatch, "observed: a check that ran keeps the mismatch wording");
        Check(ClassifySafeOff(true, true) == SafeOffKind::Mismatch, "observed late: mismatch wording");

        // The words: plain, with the seconds, and none of the mismatch wording.
        const std::wstring log = StartupWait::NotCheckedLogLine();
        const std::wstring header = StartupWait::NotCheckedHeader();
        const std::wstring summary = StartupWait::NotCheckedSummary();
        const auto has = [](const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; };
        Check(has(log, L"did not finish starting within 120 seconds") && has(log, L"could not run")
            && has(log, L"plugins are switched off for this session"), "the log line says the game did not finish starting, with the seconds");
        Check(has(header, L"did not finish starting within 120 seconds") && has(summary, L"did not finish starting within 120 seconds"),
            "the dialog says it, with the seconds");
        Check(has(header, L"message box of its own") && has(header, L"verify the game files in Steam"), "the dialog hints at a stuck game or damaged files");
        for (const std::wstring* text : { &log, &header, &summary })
            Check(!has(*text, L"mismatch") && !has(*text, L"Game layout changed") && !has(*text, L"would break your plugins"),
                "none of the mismatch wording");
    }
}

namespace
{
    // A table list with nothing but a CL, for the selection rule.
    struct ClOnly { uint32_t cl; };

    // The loader carries a table for each game build it supports, and the
    // running game's own CL picks the one it is checked against.
    void TablePerBuild()
    {
        std::printf("One table per game build\n");

        constexpr ClOnly three[] = { { 100 }, { 200 }, { 300 } };
        Check(LayoutCheck::SelectTable(three, 100) == 0 && LayoutCheck::SelectTable(three, 200) == 1
            && LayoutCheck::SelectTable(three, 300) == 2, "a build with a table gets its own");
        Check(LayoutCheck::SelectTable(three, 150) == 2 && LayoutCheck::SelectTable(three, 299) == 2,
            "a build between two tables gets the newest");
        Check(LayoutCheck::SelectTable(three, 301) == 2 && LayoutCheck::SelectTable(three, 0xFFFFFFFFu) == 2,
            "a build newer than the newest table gets the newest");
        Check(LayoutCheck::SelectTable(three, 99) == 2 && LayoutCheck::SelectTable(three, 0) == 2,
            "a build older than the oldest table, or with no CL, gets the newest");
        constexpr ClOnly one[] = { { 7 } };
        Check(LayoutCheck::SelectTable(one, 7) == 0 && LayoutCheck::SelectTable(one, 8) == 0, "a single table is always the one");

        constexpr ClOnly same[] = { { 1 }, { 1 }, { 2 } };
        constexpr ClOnly down[] = { { 2 }, { 1 } };
        constexpr ClOnly gap[] = { { 1 }, { 5 }, { 9 } };
        Check(LayoutCheck::TablesStrictlyIncreasing(gap) && LayoutCheck::TablesStrictlyIncreasing(one),
            "an increasing list passes");
        Check(!LayoutCheck::TablesStrictlyIncreasing(same) && !LayoutCheck::TablesStrictlyIncreasing(down),
            "a repeated or descending CL fails");

        using LayoutCheck::kTables;
        Check(std::size(kTables) == 2 && LayoutCheck::TablesStrictlyIncreasing(kTables),
            "the loader lists two tables, oldest first");
        Check(kTables[0].cl == 127004 && kTables[1].cl == 130045, "they are the 127004 and 130045 ones");
        Check(kTables[0].cl == SrLayoutTable_127004::kBuiltCL && kTables[1].cl == SrLayoutTable_130045::kBuiltCL,
            "each entry's CL is its table's own");

        for (const LayoutCheck::TableEntry& entry : kTables)
        {
            const std::vector<LayoutCheck::TableClass> table = entry.read();
            size_t rows = 0;
            for (const LayoutCheck::TableClass& c : table)
                rows += c.rows.size();
            std::printf("  info: CL %u: %zu classes, %zu rows\n", entry.cl, table.size(), rows);
            Check(entry.classCount > 0 && entry.rowCount > 0, "the table has classes and rows");
            Check(table.size() == entry.classCount && rows == entry.rowCount, "the counts in its header are what it holds");
        }
        Check(std::size(SrLayoutTable_127004::kClasses) == SrLayoutTable_127004::kClassCount
            && std::size(SrLayoutTable_127004::kRows) == SrLayoutTable_127004::kRowCount
            && std::size(SrLayoutTable_130045::kClasses) == SrLayoutTable_130045::kClassCount
            && std::size(SrLayoutTable_130045::kRows) == SrLayoutTable_130045::kRowCount,
            "each header's arrays are as long as its constants say");

        // What each running build is checked against.
        Check(LayoutCheck::TableCLFor(127004) == 127004 && LayoutCheck::TableCLFor(130045) == 130045,
            "127004 and 130045 each run against their own table");
        Check(LayoutCheck::TableCLFor(128786) == 130045 && LayoutCheck::TableCLFor(140000) == 130045
            && LayoutCheck::TableCLFor(120000) == 130045 && LayoutCheck::TableCLFor(0) == 130045,
            "a build with no table is checked against the newest");

        // A 127004 run selects the 127004 table, and that table is the real
        // 127004 layout (v1.26.1's table, made from the 127004 SDK): nothing
        // differs, so not even a per-build rule is needed. The 130045 table
        // on the same layout is the two-member difference that
        // NewTableOnARealLayoutOf127004 covers.
        const auto game127004 = AsGame(kLoaderTable127004());
        const auto find127004 = [&game127004](const LayoutCheck::TableClass& c) -> const LayoutCheck::LiveStruct*
        {
            const auto it = game127004.find(c.path);
            return it == game127004.end() ? nullptr : &it->second;
        };
        const LayoutCheck::TableEntry& chosen = kTables[LayoutCheck::SelectTable(kTables, 127004)];
        Check(chosen.cl == 127004, "a 127004 run selects the 127004 table");
        // v1.26.1's table lists fewer classes and members than the table does
        // now, so only those have a real 127004 layout to compare with.
        const std::vector<LayoutCheck::TableClass> chosenTable = OnlyWhatIn(chosen.read(), kLoaderTable127004());
        const LayoutCheck::Result own = LayoutCheck::Compare(chosenTable, find127004);
        Check(own.mismatched == 0 && own.missing == 0 && own.checked == own.classes && own.lines.empty()
            && !LayoutCheck::FoundChanges(own) && !LayoutCheck::Decide(chosenTable, own).safeOff,
            "its own table finds nothing changed on a 127004 layout, with no per-build rule");

        const std::vector<LayoutCheck::TableClass> newest = kTables[LayoutCheck::SelectTable(kTables, 130045)].read();
        const LayoutCheck::Result newestOnNewest = CompareWith(newest, AsGame(newest));
        Check(newestOnNewest.lines.empty() && !LayoutCheck::FoundChanges(newestOnNewest),
            "a 130045 run against the 130045 table finds nothing changed");

        // The check skips the per-build members, and a build is compared with
        // its own table, so a member that moved between the two builds is not a
        // changed class on either. What a 127004 game and a 130045 game
        // do not share is what the loader reads at one compiled offset, so
        // look at the classes whose layout a plugin keeps compiled in
        // (plugin-sdk): they must not differ between the tables except in
        // per-build members. A regeneration that moves one fails here, so it
        // is looked at before it ships. Other classes legitimately differ
        // (the tables list the same classes the sources name, and a game update
        // moves members); they are only counted.
        const std::vector<LayoutCheck::TableClass> older = kTables[0].read();
        std::map<std::string, const LayoutCheck::TableClass*> newestByPath;
        for (const LayoutCheck::TableClass& c : newest)
            newestByPath[c.path] = &c;
        int onlyInOlder = 0;
        int perBuildDifferences = 0;
        int pluginDifferences = 0;
        int otherDifferences = 0;
        int pluginClassesCompared = 0;
        for (const LayoutCheck::TableClass& a : older)
        {
            const auto found = newestByPath.find(a.path);
            if (found == newestByPath.end())
            {
                ++onlyInOlder;
                continue;
            }
            const LayoutCheck::TableClass& b = *found->second;
            const bool pluginReads = std::find(b.consumers.begin(), b.consumers.end(), LayoutCheck::kConsumerPluginSdk) != b.consumers.end();
            if (pluginReads)
                ++pluginClassesCompared;
            const auto note = [&](bool perBuild)
            {
                if (perBuild)
                    ++perBuildDifferences;
                else if (pluginReads)
                    ++pluginDifferences;
                else
                    ++otherDifferences;
            };
            if (a.sizeOf != b.sizeOf || a.alignOf != b.alignOf || a.kind != b.kind)
                note(false);
            for (const LayoutCheck::TableRow& ra : a.rows)
            {
                const LayoutCheck::TableRow* rb = nullptr;
                for (const LayoutCheck::TableRow& candidate : b.rows)
                    if (std::string(candidate.name) == ra.name)
                        rb = &candidate;
                if (rb && ra.offset == rb->offset && ra.size == rb->size && ra.boolMask == rb->boolMask)
                    continue;
                note(rb && PerBuildRule(b, *rb));
            }
            for (const LayoutCheck::TableRow& rb : b.rows)
            {
                const bool inOlder = std::any_of(a.rows.begin(), a.rows.end(),
                    [&rb](const LayoutCheck::TableRow& ra) { return std::string(ra.name) == rb.name; });
                if (!inOlder)
                    note(false);
            }
        }
        std::printf("  info: %d class(es) only in the 127004 table; %d plugin-sdk class(es) compared; the tables differ in %d per-build "
                    "member(s), %d place(s) in plugin-sdk classes and %d place(s) elsewhere\n",
            onlyInOlder, pluginClassesCompared, perBuildDifferences, pluginDifferences, otherDifferences);
        Check(onlyInOlder == 0, "the 130045 table lists every class the 127004 table does");
        Check(pluginClassesCompared > 0, "the plugin-sdk comparison saw classes, so the check below is not empty");
        Check(pluginDifferences == 0, "the tables differ in no class a plugin keeps compiled in, except in per-build members");
    }
}

namespace
{
    // The dedicated server is checked against a table made from its own SDK,
    // listing only the classes the server build reads.
    void ServerTable()
    {
        std::printf("The server's table\n");
        using LayoutCheck::kClientTables;
        using LayoutCheck::kServerTables;

        Check(std::size(kServerTables) == 1 && LayoutCheck::TablesStrictlyIncreasing(kServerTables),
            "the loader lists one server table");
        Check(kServerTables[0].cl == 127004 && kServerTables[0].cl == SrLayoutTable_Server_127004::kBuiltCL,
            "it is the server's 127004 one, with its own CL");
        Check(std::size(SrLayoutTable_Server_127004::kClasses) == SrLayoutTable_Server_127004::kClassCount
            && std::size(SrLayoutTable_Server_127004::kRows) == SrLayoutTable_Server_127004::kRowCount,
            "its arrays are as long as its constants say");

        const std::vector<LayoutCheck::TableClass> server = kServerTables[0].read();
        size_t rows = 0;
        for (const LayoutCheck::TableClass& c : server)
            rows += c.rows.size();
        std::printf("  info: server CL %u: %zu classes, %zu rows\n", kServerTables[0].cl, server.size(), rows);
        Check(server.size() == kServerTables[0].classCount && rows == kServerTables[0].rowCount && rows > 0,
            "the counts in its header are what it holds");

        // Each build reads its own list: this test defines neither build and
        // sees the client's, so the server table is not mixed into it.
        Check(std::size(kClientTables) == 2 && LayoutCheck::TableCLFor(127004) == 127004,
            "the client list is the 127004 and 130045 tables");
#if defined(MODLOADER_SERVER_BUILD)
        Check(std::size(LayoutCheck::kTables) == std::size(kServerTables), "a server build reads the server list");
#else
        Check(std::size(LayoutCheck::kTables) == std::size(kClientTables), "a client build reads the client list");
#endif

        // A server game build with a CL the table is not for, or none, is
        // checked against the only (newest) table.
        Check(LayoutCheck::SelectTable(kServerTables, 127004) == 0 && LayoutCheck::SelectTable(kServerTables, 130045) == 0
            && LayoutCheck::SelectTable(kServerTables, 0) == 0,
            "a server build with any CL gets the server table");

        // What the server reads: never a menu-layer class, and every class has
        // a consumer, so a change to any of them is judged by a tag.
        bool allTagged = true;
        bool anyMenus = false;
        for (const LayoutCheck::TableClass& c : server)
        {
            allTagged = allTagged && !c.consumers.empty();
            anyMenus = anyMenus || std::find(c.consumers.begin(), c.consumers.end(), "ui-menus") != c.consumers.end();
        }
        Check(allTagged, "every class in the server table has a consumer");
        Check(!anyMenus, "no class is tagged ui-menus: the server has no menu layer");
        Check(ClassNamed(server, "UCrUW_MapMenu") == nullptr && ClassNamed(server, "UPrimaryGameLayout") == nullptr,
            "the client's menu classes are not in it");

        // The server's own table on the layout it describes: nothing changed.
        const LayoutCheck::Result same = CompareWith(server, AsGame(server));
        Check(same.mismatched == 0 && same.missing == 0 && same.checked == same.classes && same.lines.empty()
            && !LayoutCheck::FoundChanges(same) && !LayoutCheck::Decide(server, same).safeOff,
            "its own layout finds nothing changed");

        // A member the loader's own code reads moves: safe-off, as on a client.
        const LayoutCheck::TableClass* world = ClassNamed(server, "UWorld");
        const bool worldIsCore = world && !world->rows.empty()
            && std::find(world->consumers.begin(), world->consumers.end(), "core") != world->consumers.end();
        Check(worldIsCore, "UWorld is in the server table, read by the loader's own code");
        if (worldIsCore)
        {
            auto moved = AsGame(server);
            moved[world->path].properties.front().offset += 8;
            const LayoutCheck::Result result = CompareWith(server, moved);
            const LayoutCheck::Verdict verdict = LayoutCheck::Decide(server, result);
            Check(LayoutCheck::FoundChanges(result) && verdict.safeOff && verdict.featuresOff.empty()
                && verdict.changed == std::vector<std::string>{ "UWorld" },
                "a core member moved: found, plugins switched off");

            auto gone = AsGame(server);
            gone.erase(world->path);
            const LayoutCheck::Result missing = CompareWith(server, gone);
            Check(LayoutCheck::FoundChanges(missing) && LayoutCheck::Decide(server, missing).safeOff,
                "a class the loader reads missing from the game: safe-off");
        }

        // A class only a version-map key names: listed as changed, and
        // nothing else happens.
        const LayoutCheck::TableClass* keysOnly = nullptr;
        for (const LayoutCheck::TableClass& c : server)
            if (!keysOnly && !c.rows.empty() && c.consumers == std::vector<std::string>{ "keys" })
                keysOnly = &c;
        Check(keysOnly != nullptr, "the server table has a class only a version-map key names");
        if (keysOnly)
        {
            auto moved = AsGame(server);
            moved[keysOnly->path].properties.front().offset += 8;
            const LayoutCheck::Result result = CompareWith(server, moved);
            const LayoutCheck::Verdict verdict = LayoutCheck::Decide(server, result);
            Check(LayoutCheck::FoundChanges(result) && !verdict.safeOff && verdict.featuresOff.empty()
                && verdict.changed == std::vector<std::string>{ keysOnly->name },
                "a key-only class moved: listed as changed, no safe-off");
        }
    }

    // ULevel::Actors is a TArray the engine keeps outside UPROPERTY: Dumper-7
    // writes it into the SDK, but the game's reflection data has no such
    // member. The table generator now leaves the row out (the SDK's comment
    // says NOT AUTO-GENERATED PROPERTY), so no compiled table lists it;
    // kUnreflectedMembers stays as a net for a marker the generator misses.
    // The tests below give the tables the row again, to see the net work.
    void MembersTheGameDoesNotReflect()
    {
        std::printf("A member the game does not reflect\n");
        using LayoutCheck::Kind;

        Check(LayoutCheck::FindUnreflectedMember("ULevel", "Actors") != nullptr
            && LayoutCheck::FindUnreflectedMember("ULevel", "Other") == nullptr
            && LayoutCheck::FindUnreflectedMember("AActor", "Actors") == nullptr,
            "only that class and member are listed");

        const auto listsActors = [](const std::vector<LayoutCheck::TableClass>& table)
        {
            const LayoutCheck::TableClass* level = ClassNamed(table, "ULevel");
            return level && std::any_of(level->rows.begin(), level->rows.end(),
                [](const LayoutCheck::TableRow& row) { return std::string(row.name) == "Actors"; });
        };
        const auto dropActors = [](std::map<std::string, LayoutCheck::LiveStruct>& game, const char* path)
        {
            auto& properties = game[path].properties;
            properties.erase(std::remove_if(properties.begin(), properties.end(),
                [](const LayoutCheck::LiveProperty& p) { return p.name == "Actors"; }), properties.end());
        };
        // The table as an older generator made it: ULevel with its Actors row.
        const auto withActors = [](std::vector<LayoutCheck::TableClass> table)
        {
            for (LayoutCheck::TableClass& cls : table)
                if (std::string(cls.name) == "ULevel")
                    cls.rows.push_back({ "Actors", 0xA0, 16, 0 });
            return table;
        };

        // Every compiled table is made without the row, and a game that does
        // not reflect it is clean against each, as it was with the row skipped.
        const std::vector<LayoutCheck::TableClass> server = LayoutCheck::kServerTables[0].read();
        Check(ClassNamed(server, "ULevel") != nullptr && !listsActors(server), "the server table has no ULevel::Actors row");
        for (const LayoutCheck::TableEntry& entry : LayoutCheck::kClientTables)
        {
            const std::vector<LayoutCheck::TableClass> client = entry.read();
            Check(ClassNamed(client, "ULevel") != nullptr && !listsActors(client),
                entry.cl == 127004 ? "the client CL-127004 table has no ULevel::Actors row"
                                   : "the client CL-130045 table has no ULevel::Actors row");
            auto game = AsGame(client);
            const LayoutCheck::Result result = CompareWith(client, game);
            Check(result.mismatched == 0 && result.missing == 0 && result.lines.empty() && !LayoutCheck::FoundChanges(result)
                && !LayoutCheck::Decide(client, result).safeOff && result.notReflected.empty(),
                "a client game with no ULevel::Actors: nothing changed, plugins stay on, nothing skipped");
        }

        // The net: each compiled table with the row put back, against a game
        // that reflects every member but ULevel::Actors.
        std::vector<std::vector<LayoutCheck::TableClass>> tables = { withActors(server) };
        for (const LayoutCheck::TableEntry& entry : LayoutCheck::kClientTables)
            tables.push_back(withActors(entry.read()));
        for (const std::vector<LayoutCheck::TableClass>& table : tables)
        {
            const LayoutCheck::TableClass* level = ClassNamed(table, "ULevel");
            Check(listsActors(table), "the table with the row put back lists ULevel::Actors");
            if (!level || !listsActors(table))
                continue;

            auto game = AsGame(table);
            dropActors(game, level->path);
            const LayoutCheck::Result result = CompareWith(table, game);
            Check(result.mismatched == 0 && result.missing == 0 && result.checked == result.classes && result.lines.empty()
                && result.changedClasses.empty() && !LayoutCheck::FoundChanges(result)
                && !LayoutCheck::Decide(table, result).safeOff,
                "a game with no ULevel::Actors: nothing changed, plugins stay on");
            Check(result.notReflected.size() == 1
                && result.notReflected[0] == "ULevel::Actors: not compared, "
                    "an engine TArray outside UPROPERTY, which Dumper-7 predefines at 0xA0",
                "the skipped row is listed, with its reason, for the debug log");

            // Real changes elsewhere still count, with the exempt member missing.
            const LayoutCheck::TableClass* world = ClassNamed(table, "UWorld");
            Check(world && !world->rows.empty(), "UWorld is in the table with rows");
            if (world && !world->rows.empty())
            {
                game[world->path].properties.front().offset += 8;
                const LayoutCheck::Result moved = CompareWith(table, game);
                const LayoutCheck::Verdict verdict = LayoutCheck::Decide(table, moved);
                Check(LayoutCheck::FoundChanges(moved) && verdict.safeOff && verdict.changed == std::vector<std::string>{ "UWorld" },
                    "a moved core member is still a safe-off, and ULevel is not among the changed classes");
            }
        }

        const std::vector<LayoutCheck::TableClass> sdk = {
            { "ULevel", "Engine.Level", 0, 0, Kind::Class,
                { { "Actors", 0xA0, 16, 0 }, { "Other", 0xB0, 8, 0 } } },
            { "UElse", "Mod.Else", 0, 0, Kind::Class, { { "Actors", 0xA0, 16, 0 } } },
        };

        // The same class's other members are still compared, and a member that
        // is not listed is still reported when the game lacks it.
        const std::map<std::string, LayoutCheck::LiveStruct> lacking = {
            { "Engine.Level", { 0x100, 8, { { "Other", 0xB8, 8, false, 0 } } } },
            { "Mod.Else", { 0x100, 8, {} } },
        };
        const LayoutCheck::Result partial = CompareWith(sdk, lacking);
        Check(Contains(partial, "ULevel::Other: offset SDK 0xB0, game 0xB8"),
            "another member of the same class is still compared");
        Check(!Contains(partial, "ULevel::Actors: not found in the game") && partial.notReflected.size() == 1,
            "ULevel::Actors is the one row skipped");
        Check(Contains(partial, "UElse::Actors: not found in the game"),
            "the same member name on another class is not exempt");

        // A game that does reflect it is compared like any member.
        const std::map<std::string, LayoutCheck::LiveStruct> reflected = {
            { "Engine.Level", { 0x100, 8, { { "Actors", 0xB0, 16, false, 0 }, { "Other", 0xB0, 8, false, 0 } } } },
            { "Mod.Else", { 0x100, 8, { { "Actors", 0xA0, 16, false, 0 } } } },
        };
        const LayoutCheck::Result moved = CompareWith(sdk, reflected);
        Check(Contains(moved, "ULevel::Actors: offset SDK 0xA0, game 0xB0") && moved.notReflected.empty()
            && LayoutCheck::FoundChanges(moved),
            "a reflected ULevel::Actors at another offset is a mismatch");
        const LayoutCheck::Result same = CompareWith(sdk, {
            { "Engine.Level", { 0x100, 8, { { "Actors", 0xA0, 16, false, 0 }, { "Other", 0xB0, 8, false, 0 } } } },
            { "Mod.Else", { 0x100, 8, { { "Actors", 0xA0, 16, false, 0 } } } },
        });
        Check(same.lines.empty() && same.notReflected.empty() && !LayoutCheck::FoundChanges(same),
            "a reflected ULevel::Actors at the table's offset matches");
    }
}

int main()
{
    EngineWaitDeadlineAndWording();
    PlainTypesAreToldFromMisses();
    BlueprintFunctionsPendThenResolve();
    SameBuildMatches();
    ContentUpdateMovesThePlayer();
    MismatchSwitchesPluginsOffAndReports();
    StaticArrayIsTheWholeMember();
    EachDifferenceIsReported();
    DumperRenamedMember();
    UsedMembersOnly();
    ClientTableOnEitherBuild();
    NewTableOnARealLayoutOf127004();
    OldLoaderOn130045();
    TablePerBuild();
    ServerTable();
    MembersTheGameDoesNotReflect();
    ConsumersDecideWhatStops();
    ProcessEventSlotDecidesSafeOff();
    FunctionParamsFollowTheGame();

    if (g_failures)
        std::printf("\n%d check(s) FAILED\n", g_failures);
    else
        std::printf("\nAll checks passed\n");
    return g_failures ? 1 : 0;
}
