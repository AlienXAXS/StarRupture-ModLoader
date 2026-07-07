#pragma once

// ---------------------------------------------------------------------------
// ProxyAutoUpdate -- self-update system for the mod loader, driven by the
// dwmapi.dll proxy.  Client builds only; both functions are no-ops on
// server / generic builds.
//
// The update is deliberately split across two boots so that no network or
// archive operation ever runs while the loader could be left half-updated:
//
//   Boot N   StartBackgroundCheck() (called after Core is loaded) spawns a
//            thread that fetches manifest-client.json, compares its
//            build_tag against the installed Core DLL's file version, and
//            asks the user (Yes/No message box) whether to update.  On Yes
//            the client release ZIP is downloaded, validated, and every
//            file EXCEPT dwmapi.dll is extracted into
//            ModLoader\PendingUpdate\files\ plus a pending.ini manifest.
//            Nothing that is currently running is touched.
//
//   Boot N+1 ApplyPendingUpdate() (called from DllMain BEFORE Core is
//            loaded, pure file operations only -- safe under loader lock)
//            verifies the staged files, backs up every target file, moves
//            the staged files into place, and only then deletes the
//            backups.  Any failure rolls every file back and discards the
//            staged update, leaving the previous install fully intact.
//
// Failsafes:
//   - dwmapi.dll is never written, at staging time AND at apply time
//   - ZIP entries with absolute paths, drive letters or ".." are rejected
//   - the ZIP must contain ModLoader\StarRupture-ModLoader-Core.dll or the
//     whole update is rejected
//   - pending.ini carries a Complete=1 flag written last; a partially
//     staged update (crash mid-download/extract) is detected and discarded
//   - apply is backup -> move -> verify -> delete-backups, with full
//     rollback on any failure
//   - every step logs to ModLoader\Logs\AutoUpdate.log (autoupdate_log.h)
//
// Configuration (shared with Core's updater, ModLoader\modloader.ini):
//   [AutoUpdate]
//   Enabled=1        ; 0 disables the proxy self-update check too
//   ManifestUrl=     ; overrides the compiled-in manifest URL
//
// Debug builds (_DEBUG): the pipeline is testable without CI defines -- a
// hardcoded manifest URL (the real GitHub release feed) is used when no URL
// is compiled in / configured, and the version comparison and declined-tag
// suppression are bypassed so the latest release is always offered.  The
// Yes/No prompt still appears, and download/staging/apply run exactly the
// same code as release builds.
//
// State (ModLoader\update_state.ini):
//   [AutoUpdate]
//   BuildTag=v1.15.4        ; written after an update is applied
//   ProxyDeclinedTag=v1.15.4 ; set when the user answers No, so the same
//                             version is not offered again every boot
// ---------------------------------------------------------------------------

namespace ProxyAutoUpdate
{
    // Apply a previously staged update, if any.  Must be called from
    // DllMain BEFORE the Core DLL is loaded.  Uses file operations only.
    void ApplyPendingUpdate();

    // Start the background update-check thread.  Call after Core has been
    // loaded; the thread begins running once the loader lock is released.
    void StartBackgroundCheck();
}
