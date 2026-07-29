#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Game Log Verbosity
//
// Raises (or lowers) the verbosity of the game's own UE log categories, so
// engine/game UE_LOG output that is normally filtered out reaches
// StarRupture.log.
//
// Why this exists as a hook rather than a config file: this build stages its
// entire ini hierarchy into the binary. The generated
// Saved\Config\Windows\Engine.ini is not combined into GEngineIni, so neither
// [Core.Log] nor [ConsoleVariables] placed there has any effect, and the build
// parses no -ExecCmds / -DPCVars command line either. In-process is the only
// remaining lever.
//
// Mechanism (all resolved by AOB, see scan_patterns.h):
//   FLogSuppressionImplementation keeps a static FLogCategoryBase named
//   "Global". ApplyGlobalChanges() diffs that category's Verbosity byte against
//   the value it last saw and pushes the delta out to every registered
//   category, clamping each one to its own compile-time verbosity. So writing
//   the byte and calling ApplyGlobalChanges is exactly what the engine's
//   "Log global <verbosity>" console command does, without needing to build an
//   FString or reach the console at all.
//
// Timing: FLogSuppressionImplementation::ProcessConfigAndCommandLine() applies
// [Core.Log] and -LogCmds during FEngineLoop::PreInit and would overwrite
// anything set before it. The override is therefore applied from a detour on
// that function, on return -- the earliest point that survives.
// ---------------------------------------------------------------------------

namespace Hooks::LogVerbosity
{
    // Mirrors ELogVerbosity::Type. kDefault is our own sentinel meaning
    // "leave the game's configured verbosity alone" and is not an engine value.
    enum class Level : int
    {
        Default     = -1,
        Error       = 2,
        Warning     = 3,
        Display     = 4,
        Log         = 5,
        Verbose     = 6,
        VeryVerbose = 7,
    };

    // Resolve the engine symbols and install the ProcessConfigAndCommandLine
    // detour. Reads the persisted level from modloader.ini so the override is
    // applied during startup without waiting for the UI to come up.
    // Safe to call when the setting is Default -- the detour is still installed
    // so a later live change from the settings UI has a singleton to work with.
    bool Install();

    void Remove();
    bool IsInstalled();

    // Apply a level immediately. Called by Install() (via the detour) and by the
    // settings UI when the user changes the dropdown. A Default level is a
    // no-op: it does not restore previously overridden categories, because the
    // engine's own per-category values are gone once a global change is applied.
    // Returns false if the engine symbols could not be resolved.
    bool Apply(Level level);

    // The level currently persisted / applied.
    Level GetLevel();
    void  SetLevel(Level level);

    // "Default" / "Error" / ... -- used for both the ini value and UI labels.
    const wchar_t* LevelToIniName(Level level);
    Level          LevelFromIniName(const wchar_t* name);
}

#endif // MODLOADER_CLIENT_BUILD
