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
// The command line is the exception, and the reason ApplyCommandLine() exists:
// a plugin that floods during boot has already filled the log by the time any
// UI is on screen, so there has to be a way to set a level BEFORE it loads.
// A launch argument is the right shape for that and an ini key is not -- it is
// per-run and visible in the shortcut that caused it, rather than a file whose
// contents someone has to remember editing months later.
//
//   -PluginLogLevel=<name>:<level>[,<name>:<level>...]
//
// <name> is the plugin's name as it appears in "[Plugin:NAME]" in the log --
// which is the string you are reading when you decide a plugin is too noisy.
// "*" sets the fallback for every plugin that has no entry of its own.
// <level> is trace/debug/info/warn/error, or "default" to clear.
// The argument may be repeated; quote it if a plugin name contains spaces.
//
// Thread safety: log calls arrive on every thread in the process. Reads take
// the SRW lock shared, and the common case -- no overrides set at all -- takes
// no lock and does no allocation, so the default configuration pays close to
// nothing per log line.
// ---------------------------------------------------------------------------

namespace PluginLogLevels
{
    // "No override": this plugin follows the wildcard if one is set, and
    // LogToFile::g_minLevel otherwise. Also the value every plugin has at
    // process start.
    inline constexpr int kInherit = -1;

    // Parse -PluginLogLevel= out of the process command line and apply it.
    // Call once, as early as logging is usable and before any plugin loads.
    // Repeat calls are ignored.
    void ApplyCommandLine();

    // The fallback for plugins with no override of their own ("*" on the
    // command line), or kInherit when there is none. While one is set it is
    // what "Default" resolves to, in place of LogToFile::g_minLevel.
    int  GetWildcard();
    void SetWildcard(int levelOrInherit);

    // Current override for a plugin, or kInherit. Names are matched
    // case-insensitively and are the plugin's PluginInfo name.
    int GetOverride(const char* pluginName);

    // Set (or with kInherit, remove) a plugin's override. Takes effect on the
    // next log call from that plugin.
    void SetOverride(const char* pluginName, int levelOrInherit);

    // Drop every override, including the wildcard -- the full "put it back how
    // it ships" action, which is also the only way to undo a command-line
    // wildcard without relaunching.
    void ClearAll();

    // True while anything at all is overridden (a plugin or the wildcard).
    // Lets UI say "this is not the default" without walking the plugin list.
    bool AnyOverrides();

    // The level actually in force for a plugin: its own override, else the
    // wildcard, else the global level.
    LogToFile::Level GetEffective(const char* pluginName);

    // The gate itself. Cheap enough to call on every plugin log line.
    bool ShouldLog(const char* pluginName, LogToFile::Level level);
}
