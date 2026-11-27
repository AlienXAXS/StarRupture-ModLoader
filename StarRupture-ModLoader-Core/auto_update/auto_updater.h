#pragma once

// ---------------------------------------------------------------------------
// auto_updater.h
//
// Plugin auto-update system.
//
// RunAutoUpdate() runs two independent passes at startup (before plugins load):
//
//   1. Central manifest pass
//      Fetches a JSON manifest from the modloader's own GitHub release and
//      compares build_tag against the locally stored tag.  If a newer release
//      exists the user is notified (UI overlay on client builds).  Does not
//      download anything — plugin updates are handled by the sidecar pass.
//      Skipped when no manifest URL is configured (dev / generic builds).
//
//   2. Per-plugin sidecar pass
//      Scans the Plugins directory for *.json sidecar files.  Each sidecar
//      belongs to the DLL of the same base name and contains a manifest_url
//      pointing at the plugin's own update server.  The remote manifest is
//      fetched, version compared, and the DLL downloaded if newer.
//      Runs on every boot regardless of whether a central manifest URL exists,
//      so third-party plugins can self-host their updates independently.
//
// Sidecar file format (Plugins\MyPlugin.json):
//   {
//     "manifest_url": "https://example.com/myplugin/manifest.json"
//   }
//
// Per-plugin remote manifest format:
//   {
//     "plugin_name":           "MyPlugin",
//     "version":               "1.2.0",
//     "interface_version_min": 19,
//     "interface_version_max": 22,
//     "download_url":          "https://example.com/releases/MyPlugin-1.2.0.dll"
//   }
//
// Both addresses, and every redirect of either, must be https and on an
// expected host (UpdateHosts, in http_get.h). Versions are compared by number
// (version_compare.h): when one can't be read as a number, nothing is
// downloaded.
//
// Both passes are skipped entirely when:
//   - Disabled via modloader.ini [AutoUpdate] Enabled=0
//   - Network or parse errors (failures are logged, never fatal)
//
// Per-plugin pass additionally skips a plugin when:
//   - Its declared interface_version range does not overlap [MIN, MAX]
//   - The remote manifest is unreachable, malformed or on an unexpected host
//   - The remote version is not a higher number than the installed one, or
//     either one can't be read as a number
//
// Configuration (modloader.ini):
//   [AutoUpdate]
//   Enabled=1               ; Set to 0 to disable
//   ManifestUrl=            ; Override the compiled-in central manifest URL
//   ModIndexUrl=            ; Read the Browse tab's mod index from this https URL instead
//
// State (not user-editable):
//   ModLoader\update_state.ini
//   [AutoUpdate]
//   BuildTag=v1.0.0
//   [PluginVersions]
//   MyPlugin.dll=1.2.0
// ---------------------------------------------------------------------------

// Compiled-in default manifest URL.
// Set by CI via /p:AutoUpdateManifestUrl="https://...".
// Empty string on dev / generic builds -> auto-update disabled at runtime.
#include "auto_update/loader_manifest.h"

#ifdef AUTOUPDATE_MANIFEST_URL
#  define AUTOUPDATE_DEFAULT_MANIFEST_URL AUTOUPDATE_MANIFEST_URL
#else
#  define AUTOUPDATE_DEFAULT_MANIFEST_URL ""
#endif

// The latest loader release the central manifest named (its tag and the plugin
// interface range it supports). False, with an empty result, when the manifest
// was not read: no URL, no connection, or updates off. Safe from any thread.
bool GetLatestLoaderRelease(LoaderManifest::Latest& out);

// Whether modloader.ini [AutoUpdate] Enabled is on (the default).
bool AutoUpdateEnabled();

// Records the version a plugin with an update sidecar reports when it loads,
// if it is newer than the stored one, so the plugin pass never replaces a
// build installed by hand with an older release. The pass reads the installed
// DLL itself first; this covers a DLL it could not read. dllFileName is the bare file
// name ("MyPlugin.dll").
void NoteInstalledPluginVersion(const wchar_t* dllFileName, const char* version);

// Replaces the stored version, whatever it was. For a DLL the player just
// installed: an older record left by an uninstalled copy must not outlive it.
void SetInstalledPluginVersion(const wchar_t* dllFileName, const char* version);

namespace ModLoaderLogger
{
	// Fetch the remote manifest, compare build_tag against update_state.ini,
	// and download any outdated or missing plugin DLLs into the Plugins dir.
	// Silent on any network / parse error.  Must be called before LoadAllPlugins().
	void RunAutoUpdate();
}
