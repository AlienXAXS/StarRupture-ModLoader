#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VersionMap { struct LiveField; }

// Startup layout check.
//
// layout_table.generated.h lists the Game-SDK classes the loader uses, with
// the size of each and the offset, size and bool mask of the members it
// reads, as the SDK the loader was built against has them
// (tools\Update-LayoutTable.ps1 regenerates it). Run() walks the running
// game's reflection data once and logs every class whose layout no longer
// matches: after a game update that is the list of SDK types that now read
// the wrong bytes. Members the loader resolves per build
// (SdkGlobals::ResolveFields) are not compared here: the version map checks
// those (VersionMap::SelectField).
//
// Each class in the table is tagged with what reads its layout (its
// consumers). On a mismatch Decide says what has to stop: a loader feature
// when only that feature reads the class; all plugins (safe-off) when the
// loader itself or every plugin reads it, or when nothing says who does.
// Plugins that keep the game's layouts compiled in are refused whenever any
// class changed (VersionMap::LayoutRefusal). The init thread acts on the
// verdict and writes the compatibility report (compat_report.h).
//
// Run also checks the one engine constant every plugin's SDK compiles in: the
// vtable slot UObject::ProcessEvent is called through (CheckProcessEventSlot).
namespace LayoutCheck
{
    // Called once from the init thread after FEngineLoop::Init has returned,
    // while the game thread is still parked in the engine-init hook and before
    // any plugin loads. Client builds only: the table is the client SDK's.
    // Without MODLOADER_CLIENT_BUILD or MODLOADER_SERVER_BUILD (the offline
    // test) only the comparison below is compiled.
    //
    // False, and logged, when the check could not run (GObjects invalid, an
    // exception while reading reflection data); out is then left as it was.
    struct Result;
    bool Run(Result& out);

    // The reflected layout of one member of a native class or script struct
    // (member empty: the struct's own size), for field requests
    // (VersionMap::SetLiveFieldSource). The member is looked up in the struct
    // and then in its supers. className is the short name, or
    // /Script/<Module>.<Name>; a short name more than one module has is an
    // error. Client and server builds, once engine init has been seen.
    //
    // className may also be a Dumper-7 parameter struct, <Class>_<Function>
    // (CrEnviroWaveSubsystem_GetCurrentStageSettings, no prefix letter) or
    // /Script/<Module>.<Class>_<Function>. The member is then one of the
    // UFunction's parameters (ReturnValue for the return) at its offset in the
    // parameter block, and the size (member empty) is the function's
    // ParmsSize. See ResolveParamMember.
    VersionMap::LiveField LiveFieldOf(const std::string& className, const std::string& member);

    // ---- The comparison, kept apart from the game so it can be tested ------

    // Class::kind in the generated table.
    enum class Kind : uint32_t
    {
        Unknown        = 0,
        Class          = 1,
        ScriptStruct   = 2,
        FunctionParams = 3,
        Blueprint      = 4,
    };

    struct TableRow
    {
        const char* name;
        uint32_t    offset;
        uint32_t    size;       // 0: unknown, not compared
        uint32_t    boolMask;   // 0: not a bool
    };

    struct TableClass
    {
        const char*           name;     // the SDK's C++ name, ACrCharacterPlayerBase
        const char*           path;     // Module.Name, or Module.Class.Function for Kind::FunctionParams
        uint32_t              sizeOf;
        uint32_t              alignOf;  // 0: unknown; never compared for Kind::FunctionParams
        Kind                  kind;
        std::vector<TableRow> rows;
        std::vector<std::string> consumers;  // what reads the layout (kConsumer*); empty = not known
    };

    // Consumer ids (tools\Update-LayoutTable.ps1 tags the table with them).
    inline constexpr const char* kConsumerCore      = "core";        // the loader's own code
    inline constexpr const char* kConsumerPluginSdk = "plugin-sdk";  // compiled into every plugin
    inline constexpr const char* kConsumerUiMenus   = "ui-menus";    // the menu-layer read only
    inline constexpr const char* kConsumerKeys      = "keys";        // named only as a version-map or pattern key

    // Reads a generated table: ReadTable(SrLayoutTable::kNames,
    // SrLayoutTable::kClasses, SrLayoutTable::kRows).
    template <class ClassT, size_t ClassCount, class RowT, size_t RowCount, size_t NameCount>
    std::vector<TableClass> ReadTable(const char* const (&names)[NameCount],
        const ClassT (&classes)[ClassCount], const RowT (&rows)[RowCount])
    {
        const auto name = [&](uint32_t index) { return index < NameCount ? names[index] : ""; };

        std::vector<TableClass> table;
        table.reserve(ClassCount);
        for (const ClassT& c : classes)
        {
            TableClass& out = table.emplace_back(TableClass{ name(c.name), name(c.path), c.sizeOf, c.alignOf,
                static_cast<Kind>(c.kind), {} });
            for (uint32_t i = c.firstRow; i < c.firstRow + c.rowCount && i < RowCount; ++i)
                out.rows.push_back(TableRow{ name(rows[i].name), rows[i].offset, rows[i].size, rows[i].boolMask });
        }
        return table;
    }

    // A generated table with its consumer tags: ReadTable(..., SrLayoutTable::kConsumers,
    // SrLayoutTable::kClassConsumers). A bit with no id gives "?", which no
    // rule knows, so that class counts as not attributed.
    template <class ClassT, size_t ClassCount, class RowT, size_t RowCount, size_t NameCount, size_t ConsumerCount>
    std::vector<TableClass> ReadTable(const char* const (&names)[NameCount],
        const ClassT (&classes)[ClassCount], const RowT (&rows)[RowCount],
        const char* const (&consumers)[ConsumerCount], const uint32_t (&classConsumers)[ClassCount])
    {
        std::vector<TableClass> table = ReadTable(names, classes, rows);
        for (size_t i = 0; i < ClassCount; ++i)
            for (uint32_t bit = 0; bit < 32; ++bit)
                if ((classConsumers[i] >> bit) & 1u)
                    table[i].consumers.push_back(bit < ConsumerCount && consumers[bit] ? consumers[bit] : "?");
        return table;
    }

    // What the game reflects for one struct.
    struct LiveProperty
    {
        std::string name;
        int32_t     offset;
        int32_t     size;       // ElementSize * ArrayDim
        bool        isBool;
        uint32_t    fieldMask;  // FBoolProperty::FieldMask when isBool
    };

    struct LiveStruct
    {
        int32_t                   size;          // UStruct::PropertiesSize
        int32_t                   minAlignment;
        std::vector<LiveProperty> properties;    // its own ChildProperties, not its super's
    };

    // Change::field for a class or member the game no longer has.
    inline constexpr const char* kChangeMissing = "missing";

    // Change::field for a vtable slot that holds another function than the
    // map's (class "UObject", member "ProcessEvent"): sdk is the function's
    // offset in the image as the map has it, game the slot's.
    inline constexpr const char* kChangeVtableSlot = "vtableSlot";

    // One difference, for the compatibility report.
    struct Change
    {
        std::string className;  // the SDK's C++ name
        std::string member;     // empty: the class itself
        std::string field;      // kChangeMissing, "size", "alignment", "offset", "boolMask" or "isBool"
        uint32_t    sdk;        // the SDK's value (isBool: 1 or 0)
        int32_t     game;       // the game's value
    };

    // What a set of changes switches off.
    struct Verdict
    {
        bool                     safeOff = false;  // every plugin off (see Decide)
        std::vector<std::string> featuresOff;      // loader features to switch off (kConsumerUiMenus)
        std::vector<std::string> changed;          // SDK names of the changed classes
        std::vector<std::string> changedPaths;     // their table paths, in the same order
    };

    struct Result
    {
        int classes    = 0;  // in the table
        int perBuild   = 0;  // rows left to the per-build resolver, not compared
        int checked    = 0;  // found in the game and compared
        int mismatched = 0;  // found, but the layout differs
        int missing    = 0;  // not found in the game
        int slotMismatched = 0;  // vtable slots that do not hold the function the map resolved
        int deferred   = 0;  // not checked at startup (Blueprint classes, unknown kind)
        std::vector<std::string> changedClasses;  // SDK names of the mismatched and missing classes
        std::vector<std::string> lines;           // one per difference, ready to log
        std::vector<Change>      changes;         // the same differences, one each
        Verdict                  verdict;         // Run: Decide's answer for these changes
    };

    // A class the game no longer has, or one whose layout moved: the SDK reads
    // the wrong bytes there. Or a ProcessEvent slot that moved: every plugin
    // then calls the wrong function.
    inline bool FoundChanges(const Result& result)
    {
        return result.mismatched > 0 || result.missing > 0 || result.slotMismatched > 0;
    }

    // What the changes in `result` switch off, by the consumers `table` tags
    // each changed class with:
    //   ui-menus  that loader feature;
    //   keys      nothing in the loader (plugins: VersionMap::LayoutRefusal);
    //   core, plugin-sdk, an id not listed here, no id at all, or a class not
    //   in the table: safe-off.
    // A difference that names no changed class is safe-off too.
    Verdict Decide(const std::vector<TableClass>& table, const Result& result);

    // Every plugin's Game-SDK calls UObject::ProcessEvent through slot
    // Offsets::ProcessEventIdx of the object's vtable (the index is compiled
    // in), and the loader hooks the function the version map resolves. Both
    // must be the same function. `slot` is what the UObject class default
    // object's vtable holds at `index` (ReadVtableSlot), `index` the SDK's
    // slot, `resolved` the map-resolved address and
    // `imageBase` the main module's base (the lines show offsets in the
    // image). A slot that holds another address adds a difference to `result`
    // (Decide switches all plugins off, whatever else changed) and returns
    // false.
    bool CheckProcessEventSlot(Result& result, uintptr_t slot, int32_t index,
        uintptr_t resolved, uintptr_t imageBase);

    // Reads entry `index` of `vtable` under SEH. A vtable that is short or
    // not a vtable (the case the slot check exists for) faults on the read:
    // that returns false with the exception code and `slot` untouched, and the
    // caller treats it as "cannot verify", not as a mismatch.
    bool ReadVtableSlot(const uintptr_t* vtable, int32_t index, uintptr_t& slot, uint32_t& exceptionCode);

    // Compares every table class with what `find` returns for it (nullptr:
    // not in the game). A size or alignment of 0 is not compared: in a
    // granularity-1 table a class lists only the members the loader reads,
    // and a UClass the loader reaches only through pointers has size 0 (one
    // it allocates or copies, such as UFunction, keeps its size). Rows for
    // which perBuild is true are skipped and counted in Result::perBuild.
    // A row Dumper-7 renamed with a _<n> suffix (Class_0) matches the game's
    // member without it only at the same offset and size.
    using PerBuild = std::function<bool(const TableClass&, const TableRow&)>;
    Result Compare(const std::vector<TableClass>& table,
        const std::function<const LiveStruct*(const TableClass&)>& find,
        const PerBuild& perBuild = {});

    // ---- Function parameter blocks, kept apart from the game so they can be tested ----

    // One way to read a parameter struct name as <Class>_<Function>. Both
    // halves may hold underscores (SetInputMode_GameAndUIEx), so every
    // underscore is a candidate split.
    struct ParamSplit
    {
        std::string module;     // from /Script/<Module>.; empty = any module
        std::string owner;      // the class, as the game names it (CrEnviroWaveSubsystem)
        std::string function;
    };
    std::vector<ParamSplit> SplitParamStruct(const std::string& name);

    // A UFunction as the game reflects it: its parameters and return value (the
    // properties flagged as parameters, not the locals of a Blueprint function).
    struct LiveFunction
    {
        std::string               path;    // Module.Class.Function
        std::vector<LiveProperty> params;
    };

    // The function's ParmsSize: the end of its last parameter, as the engine
    // computes it when it links the function (not rounded up to an alignment).
    uint32_t ParmsSizeOf(const LiveFunction& function);

    struct ParamMember
    {
        std::string error;      // set when there is no answer
        std::string path;       // Module.Class.Function of the function that answered
        int32_t     offset = 0;
        uint32_t    size = 0;
        bool        isBool = false;
        uint32_t    boolMask = 0;
    };

    // The answer for a request against a parameter struct, given every
    // function its name can mean (the game side collects them through
    // SplitParamStruct). member empty: the block size, ParmsSizeOf. Refused
    // when no function or more than one carries the name, and when the function
    // has no such parameter. A member under Dumper-7's _<n> suffix for a name it
    // reserves (Class_0) is found under the name without it.
    ParamMember ResolveParamMember(const std::string& name, const std::vector<LiveFunction>& functions,
        const std::string& member);
}
