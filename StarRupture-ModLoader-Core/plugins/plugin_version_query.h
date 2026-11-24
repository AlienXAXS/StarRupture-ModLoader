#pragma once

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// plugin_version_query.h -- "is that plugin running, and which version?", as
// plain data.
//
// IPluginRegistry::GetPluginVersion answers it for a plugin. The plugin table
// belongs to plugin_manager.cpp and is guarded by its lock; this header holds
// only the rule, with no game, loader or Windows types, so it can be tested on
// its own. The caller holds the table's lock around Find.
//
// A plugin counts only while it is running. A plugin that is not in the table,
// is disabled, was unloaded (the overlay's RELOAD button unloads before it loads
// again), or is part-way through a shutdown or reload is "not loaded". The name
// compares without regard to case, as the plugin list and the ini do.
// ---------------------------------------------------------------------------
namespace PluginVersionQuery
{
    // What Find needs to know about one entry of the plugin table.
    struct Row
    {
        const std::string* name;     // PluginInfo name
        const std::string* version;  // PluginInfo version
        bool               running;  // initialized and not being shut down
    };

    // True, with the plugin's version in out (NUL-terminated), for the first
    // running entry whose name matches query. False when none does, when query is
    // null or empty, or when out is too small for the whole version: out is then
    // left as an empty string, never a cut version. out may be null (cap ignored)
    // to ask only whether the plugin is running. `project` turns one element of
    // `all` into a Row.
    template <class Container, class Project>
    bool Find(const Container& all, Project project, const char* query, char* out, size_t cap)
    {
        if (out && cap > 0)
            out[0] = '\0';
        if (!query || !*query)
            return false;

        for (const auto& entry : all)
        {
            const Row row = project(entry);
            if (!row.running || !row.name || _stricmp(row.name->c_str(), query) != 0)
                continue;
            if (!out)
                return true;
            const size_t need = row.version ? row.version->size() + 1 : 1;
            if (cap < need)
                return false;
            std::memcpy(out, row.version ? row.version->c_str() : "", need);
            return true;
        }
        return false;
    }
}
