#pragma once

// ---------------------------------------------------------------------------
// UpdaterLaunch -- proxy-side glue for the self-update system.
//
// The update check itself lives in a separate process,
// ModLoader\StarRupture-ModLoader-Updater.exe (see the
// StarRupture-ModLoader-Updater project), because network APIs cannot be
// used safely inside DllMain (loader lock).  Spawning a child process and
// waiting for it IS safe there: the child has its own loader.
//
// RunUpdaterAndWait() is called from the proxy's DllMain BEFORE the Core
// DLL is loaded, so an accepted update replaces the Core DLL (and friends)
// on disk and the same game session then loads the fresh version -- no
// second launch required.  dwmapi.dll itself is never touched.
//
// Failsafes:
//   - updater exe missing -> logged, game boots normally
//   - updater crash / abnormal exit -> logged, game boots normally with
//     whatever is on disk (the updater's own rollback guarantees that is
//     a complete install)
//   - the updater exe's leftover self-update backup
//     (StarRupture-ModLoader-Updater.exe.autoupdate.bak, undeletable while
//     the exe was running) is cleaned up here on the next boot
//
// Client builds only; no-op on server / generic builds.
// ---------------------------------------------------------------------------

namespace UpdaterLaunch
{
    // Spawns the updater exe and blocks until it exits.  Never throws or
    // fails the boot; all outcomes are logged to AutoUpdate.log.
    void RunUpdaterAndWait();
}
