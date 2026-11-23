#pragma once

#include <iterator>

#include "layout_check.h"
#include "layout_table_127004.generated.h"
#include "layout_table_130045.generated.h"
#include "layout_table_server_127004.generated.h"

// The layout tables the loader carries, one per game build it checks, oldest
// first (tools\Update-LayoutTable.ps1 writes each into its own file and
// namespace). The running game's own CL picks the table it is checked against
// (LayoutCheck::SelectTable); a build that has none is checked against the
// newest, which is the table of the SDK the loader is built against
// (VersionMap::kBuiltCL, asserted in layout_check.cpp). A table for a new
// build is listed here (include, Read function, kClientTables entry) and in the
// Core and LayoutCheckTests projects.
//
// The dedicated server is another game build with its own SDK, so it has its
// own list, kServerTables (tools\Update-LayoutTable.ps1 -Server). kTables is the
// list of the build being compiled; the offline test, which defines neither
// build, sees the client's.
static_assert(SrLayoutTable_127004::kFormatVersion == 1 && SrLayoutTable_130045::kFormatVersion == 1
        && SrLayoutTable_Server_127004::kFormatVersion == 1,
    "layout_check.cpp reads layout table format 1");
static_assert(SrLayoutTable_127004::kDetail == 1 && SrLayoutTable_130045::kDetail == 1
        && SrLayoutTable_Server_127004::kDetail == 1,
    "an embedded layout table needs member sizes and bool masks (generate it with --sdk)");

namespace LayoutCheck
{
    struct TableEntry
    {
        uint32_t                 cl;
        uint32_t                 classCount;  // the table's own kClassCount
        uint32_t                 rowCount;    // the table's own kRowCount
        std::vector<TableClass> (*read)();
    };

    namespace tables_detail
    {
        inline std::vector<TableClass> Read127004()
        {
            return ReadTable(SrLayoutTable_127004::kNames, SrLayoutTable_127004::kClasses, SrLayoutTable_127004::kRows,
                SrLayoutTable_127004::kConsumers, SrLayoutTable_127004::kClassConsumers);
        }

        inline std::vector<TableClass> Read130045()
        {
            return ReadTable(SrLayoutTable_130045::kNames, SrLayoutTable_130045::kClasses, SrLayoutTable_130045::kRows,
                SrLayoutTable_130045::kConsumers, SrLayoutTable_130045::kClassConsumers);
        }

        inline std::vector<TableClass> ReadServer127004()
        {
            return ReadTable(SrLayoutTable_Server_127004::kNames, SrLayoutTable_Server_127004::kClasses, SrLayoutTable_Server_127004::kRows,
                SrLayoutTable_Server_127004::kConsumers, SrLayoutTable_Server_127004::kClassConsumers);
        }
    }

    // Strictly increasing by CL: the last is the newest.
    inline constexpr TableEntry kClientTables[] = {
        { SrLayoutTable_127004::kBuiltCL, SrLayoutTable_127004::kClassCount, SrLayoutTable_127004::kRowCount, tables_detail::Read127004 },
        { SrLayoutTable_130045::kBuiltCL, SrLayoutTable_130045::kClassCount, SrLayoutTable_130045::kRowCount, tables_detail::Read130045 },
    };

    // The server's, made from the Server SDK: the server build is checked against
    // these, never the client's, whose classes and sizes differ.
    inline constexpr TableEntry kServerTables[] = {
        { SrLayoutTable_Server_127004::kBuiltCL, SrLayoutTable_Server_127004::kClassCount, SrLayoutTable_Server_127004::kRowCount, tables_detail::ReadServer127004 },
    };

    static_assert(TablesStrictlyIncreasing(kClientTables), "kClientTables must list the tables oldest first, one per CL");
    static_assert(TablesStrictlyIncreasing(kServerTables), "kServerTables must list the tables oldest first, one per CL");

    // The tables of the build being compiled.
#if defined(MODLOADER_SERVER_BUILD)
    inline constexpr const auto& kTables = kServerTables;
#else
    inline constexpr const auto& kTables = kClientTables;
#endif

    // The CL of the table a game build is checked against.
    inline constexpr uint32_t TableCLFor(uint32_t runningCL)
    {
        return kTables[SelectTable(kTables, runningCL)].cl;
    }
}
