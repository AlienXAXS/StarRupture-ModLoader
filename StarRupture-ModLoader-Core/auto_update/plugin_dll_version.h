#pragma once

#include "plugins/plugin_interface.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// The version a plugin DLL reports for itself (PluginInfo::version), read
// without starting the plugin. The plugin update pass runs before the plugins
// are loaded, so this is how it learns what is installed, whether the DLL came
// from the updater or was put there by hand.
namespace PluginDllVersion
{
    // GetPluginInfo returns a pointer to a table in the DLL's own image. The
    // call is guarded: a DLL that was not mapped to run (see Read) may fault.
    inline bool CopyReportedVersion(HMODULE module, char* out, size_t outSize)
    {
        const auto getInfo = reinterpret_cast<GetPluginInfoFunc>(
            GetProcAddress(module, PLUGIN_GET_INFO_FUNC_NAME));
        if (!getInfo)
            return false;

        __try
        {
            const PluginInfo* info = getInfo();
            if (!info || !info->version || !info->version[0])
                return false;
            strncpy_s(out, outSize, info->version, _TRUNCATE);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The version the DLL at path reports, or "" if it cannot be read (no such
    // file, not a plugin, GetPluginInfo needs more than a mapped image).
    //
    // The image is mapped with DONT_RESOLVE_DLL_REFERENCES: relocated, but no
    // imports resolved, no DllMain and no static initialisers run, so nothing
    // of the plugin starts. A plugin's GetPluginInfo returns a constant table,
    // which needs none of that.
    inline std::string Read(const wchar_t* path)
    {
        HMODULE module = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!module)
            return {};

        char version[128] = {};
        const bool ok = CopyReportedVersion(module, version, sizeof(version));
        FreeLibrary(module);
        return ok ? std::string(version) : std::string();
    }
}
