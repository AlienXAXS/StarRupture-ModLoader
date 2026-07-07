#pragma once

// ---------------------------------------------------------------------------
// StarRupture-ModLoader-Updater -- standalone helper exe that self-updates
// the mod loader BEFORE the Core DLL is loaded.  Client builds only; on
// server / generic builds the exe compiles to a no-op.
//
// Why a separate process: the dwmapi.dll proxy decides whether to update
// inside DllMain, which runs under the Windows loader lock.  Network APIs
// (WinHTTP) can deadlock there, and threads spawned from DllMain cannot be
// waited on.  A child process has its own loader, so the proxy can safely
// CreateProcess this exe and block until it exits, then load the (possibly
// freshly updated) Core DLL.
//
// Single-boot flow (proxy DllMain -> this exe -> proxy continues):
//   1. Read the installed version from the Core DLL's version resource on
//      disk (no DLL load), falling back to update_state.ini BuildTag, then
//      the compiled-in build tag.
//   2. Fetch manifest-client.json and compare build_tag.
//   3. Up to date -> exit; the proxy loads Core immediately.
//   4. Update available -> Yes/No message box.  On Yes: download the client
//      release ZIP, validate it, extract it fully into memory, then install
//      every file EXCEPT dwmapi.dll directly into the game directory using
//      backup -> move -> delete-backups with full rollback on any failure.
//      The Core DLL is not loaded yet, so nothing (except dwmapi.dll, which
//      is skipped anyway) is file-locked.
//   5. Exit; the proxy loads the new Core DLL in the same game session.
//
// Failsafes:
//   - dwmapi.dll is never written (it is locked by the game anyway)
//   - ZIP entries with absolute paths, drive letters or ".." are rejected
//   - the ZIP must contain ModLoader\StarRupture-ModLoader-Core.dll or the
//     whole update is rejected
//   - the entire archive is downloaded and decompressed in memory before
//     the first file on disk is touched -- a truncated download or corrupt
//     archive can never produce a half-written install
//   - install is backup -> move -> delete-backups with rollback on failure
//   - this exe replaces itself via the rename trick (a running exe can be
//     renamed); its leftover .autoupdate.bak is cleaned by the proxy on the
//     next boot
//   - every step is appended to ModLoader\Logs\AutoUpdate.log (the proxy
//     owns rotation; see autoupdate_log.h in the proxy project)
//
// Debug builds (_DEBUG): a hardcoded manifest URL (the real GitHub release
// feed) is used when none is compiled in / configured, and the version
// comparison and declined-tag suppression are bypassed so the latest
// release is always offered.  Download and install run the same code as
// release builds.
//
// Configuration (shared with Core's updater, ModLoader\modloader.ini):
//   [AutoUpdate]
//   Enabled=1        ; 0 disables the self-update check too
//   ManifestUrl=     ; overrides the compiled-in manifest URL
//
// State (ModLoader\update_state.ini):
//   [AutoUpdate]
//   BuildTag=v1.15.4         ; written after an update is installed
//   ProxyDeclinedTag=v1.15.4 ; set when the user answers No, so the same
//                              version is not offered again every boot
// ---------------------------------------------------------------------------

// Exit codes returned to the proxy (informational; the proxy always
// continues to load Core regardless).
enum UpdaterExitCode
{
    UPDATER_EXIT_UP_TO_DATE = 0, // no update available / check skipped
    UPDATER_EXIT_UPDATED    = 1, // update installed successfully
    UPDATER_EXIT_DECLINED   = 2, // update available but user said No
    UPDATER_EXIT_FAILED     = 3, // update attempted but failed (rolled back)
};

// Runs the whole check/prompt/download/install flow.  gameDir is the game's
// Binaries\Win64 directory (no trailing backslash).
int RunUpdater(const wchar_t* gameDir);
