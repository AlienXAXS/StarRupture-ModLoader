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
// On a mismatch the init thread switches plugins off (RequiresSafeOff) and
// writes the compatibility report (compat_report.h).
namespace LayoutCheck
{
    // Called once from the init thread after FEngineLoop::Init has returned,
    // while the game thread is still parked in the engine-init hook and before
    // any plugin loads. Client builds only: layout_check.cpp is not compiled
    // into the server build, and without MODLOADER_CLIENT_BUILD (the offline
    // test) only the comparison below is.
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
    // error. Client builds only, and only once engine init has been seen.
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
    };

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

    // One difference, for the compatibility report.
    struct Change
    {
        std::string className;  // the SDK's C++ name
        std::string member;     // empty: the class itself
        std::string field;      // kChangeMissing, "size", "alignment", "offset", "boolMask" or "isBool"
        uint32_t    sdk;        // the SDK's value (isBool: 1 or 0)
        int32_t     game;       // the game's value
    };

    struct Result
    {
        int classes    = 0;  // in the table
        int perBuild   = 0;  // rows left to the per-build resolver, not compared
        int checked    = 0;  // found in the game and compared
        int mismatched = 0;  // found, but the layout differs
        int missing    = 0;  // not found in the game
        int deferred   = 0;  // not checked at startup (Blueprint classes, unknown kind)
        std::vector<std::string> changedClasses;  // SDK names of the mismatched and missing classes
        std::vector<std::string> lines;           // one per difference, ready to log
        std::vector<Change>      changes;         // the same differences, one each
    };

    // A class the game no longer has, or one whose layout moved, means the SDK
    // reads the wrong bytes there: plugins must not run.
    inline bool RequiresSafeOff(const Result& result)
    {
        return result.mismatched > 0 || result.missing > 0;
    }

    // Compares every table class with what `find` returns for it (nullptr:
    // not in the game). A size or alignment of 0 is not compared: in a
    // granularity-1 table a class lists only the members the loader reads,
    // and a UClass the loader reaches only through pointers has size 0 (one
    // it allocates or copies, such as UFunction, keeps its size). Rows for
    // which perBuild is true are skipped and counted in Result::perBuild.
    using PerBuild = std::function<bool(const TableClass&, const TableRow&)>;
    Result Compare(const std::vector<TableClass>& table,
        const std::function<const LiveStruct*(const TableClass&)>& find,
        const PerBuild& perBuild = {});
}
