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

    // The loader's own table, with what reads each class.
    const auto kLoaderTable = []
    {
        return LayoutCheck::ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows,
            SrLayoutTable::kConsumers, SrLayoutTable::kClassConsumers);
    };

    const LayoutCheck::TableClass* ClassNamed(const std::vector<LayoutCheck::TableClass>& table, const char* name)
    {
        for (const LayoutCheck::TableClass& c : table)
            if (std::string(c.name) == name)
                return &c;
        return nullptr;
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
        Check(narrow.mismatched == 3 && narrow.lines.size() == 3 && LayoutCheck::FoundChanges(narrow), "nothing else");

        const LayoutCheck::Result whole = LayoutCheck::Compare(sdk, find);
        Check(Contains(whole, "UPerBuild::Resolved: offset SDK 0x30, game 0x48"), "without a per-build rule every row is compared");
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
        const auto table = LayoutCheck::ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows);
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

int main()
{
    BlueprintFunctionsPendThenResolve();
    SameBuildMatches();
    ContentUpdateMovesThePlayer();
    MismatchSwitchesPluginsOffAndReports();
    StaticArrayIsTheWholeMember();
    EachDifferenceIsReported();
    DumperRenamedMember();
    UsedMembersOnly();
    ConsumersDecideWhatStops();
    ProcessEventSlotDecidesSafeOff();
    FunctionParamsFollowTheGame();

    if (g_failures)
        std::printf("\n%d check(s) FAILED\n", g_failures);
    else
        std::printf("\nAll checks passed\n");
    return g_failures ? 1 : 0;
}
