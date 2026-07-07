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
// second launch required.  dwmapi.dll and the updater exe are also
// replaced (via rename, since they are in use); those two take effect on
// the next launch.
//
// Failsafes:
//   - updater exe missing -> logged, game boots normally
//   - updater crash / abnormal exit -> logged, game boots normally with
//     whatever is on disk (the updater's own rollback guarantees that is
//     a complete install)
//   - leftover *.autoupdate.bak files (backups of binaries that were in
//     use during the previous update and could not be deleted then) are
//     swept from the game dir and ModLoader\ here on the next boot
//
// Client builds only; no-op on server / generic builds.
// ---------------------------------------------------------------------------

namespace UpdaterLaunch
{
    // Spawns the updater exe and blocks until it exits.  Never throws or
    // fails the boot; all outcomes are logged to AutoUpdate.log.
    void RunUpdaterAndWait();
}
