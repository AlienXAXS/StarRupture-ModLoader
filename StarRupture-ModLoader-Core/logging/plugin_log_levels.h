#pragma once

#include "log.h"

// ---------------------------------------------------------------------------
// Per-plugin log levels.
//
// A single global minimum level is the wrong tool when one plugin is being
// debugged: turning the loader down to TRACE to chase one plugin buries it
// under every other plugin's chatter, and turning it back up hides the thing
// you were looking for. This registry gives each plugin its own minimum,
// applied where the plugin's log call enters the loader (logger.cpp's
// PluginLog).
//
// A plugin's override REPLACES the global gate rather than stacking with it --
// see LogToFile::WriteBypass. Stacking would make "set MyPlugin to Trace"
// silently do nothing whenever the global level was higher, which is exactly
// the case the feature exists for.
//
// NOT PERSISTED, deliberately. There is no ini section for this and there must
// not be one: an override is a debugging tool, and one left behind in a file
// months ago silently swallowing a plugin's errors is a far worse outcome than
// having to set it again. Every plugin starts each launch on kInherit, so a
// fresh process logs everything at the level chosen in modloader.ini.
//
// Thread safety: log calls arrive on every thread in the process. Reads take
// the SRW lock shared, and the common case -- no overrides set at all -- takes
// no lock and does no allocation, so the default configuration pays close to
// nothing per log line.
// ---------------------------------------------------------------------------

namespace PluginLogLevels
{
    // "No override": this plugin follows LogToFile::g_minLevel, whatever that
    // currently is. Also the value every plugin has at process start.
    inline constexpr int kInherit = -1;

    // Current override for a plugin, or kInherit. Names are matched
    // case-insensitively and are the plugin's PluginInfo name.
    int GetOverride(const char* pluginName);

    // Set (or with kInherit, remove) a plugin's override. Takes effect on the
    // next log call from that plugin.
    void SetOverride(const char* pluginName, int levelOrInherit);

    // Drop every override -- equivalent to setting them all to kInherit.
    void ClearAll();

    // True while at least one plugin has an override set. Lets UI say "this is
    // not the default" without walking the plugin list.
    bool AnyOverrides();

    // The level actually in force for a plugin: its override, or the global
    // level when it has none.
    LogToFile::Level GetEffective(const char* pluginName);

    // The gate itself. Cheap enough to call on every plugin log line.
    bool ShouldLog(const char* pluginName, LogToFile::Level level);
}
