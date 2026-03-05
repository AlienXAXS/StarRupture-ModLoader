#pragma once

// ---------------------------------------------------------------------------
// auto_updater.h
//
// Plugin auto-update system.
//
// At startup (before LoadAllPlugins), RunAutoUpdate() fetches a JSON manifest
// from GitHub Releases, compares the build_tag against the locally stored
// tag in Plugins\update_state.ini, and downloads any outdated or missing
// plugin DLLs into the Plugins directory.
//
// Auto-update is skipped entirely when:
//   - Disabled via modloader.ini [AutoUpdate] Enabled=0
//   - No manifest URL is configured (dev / generic builds have no URL)
//   - The manifest's interface_version != PLUGIN_INTERFACE_VERSION
//   - Network or parse errors (failures are logged, never fatal)
//
// Configuration (modloader.ini):
//   [AutoUpdate]
//   Enabled=1               ; Set to 0 to disable
//   ManifestUrl=            ; Override the compiled-in default URL
//
// State (not user-editable):
//   Plugins\update_state.ini
//   [AutoUpdate]
//   BuildTag=release-2026.03.04-120000
// ---------------------------------------------------------------------------

// Compiled-in default manifest URL.
// Set by CI via /p:AutoUpdateManifestUrl="https://...".
// Empty string on dev / generic builds -> auto-update disabled at runtime.
#ifdef AUTOUPDATE_MANIFEST_URL
#  define AUTOUPDATE_DEFAULT_MANIFEST_URL AUTOUPDATE_MANIFEST_URL
#else
#  define AUTOUPDATE_DEFAULT_MANIFEST_URL ""
#endif

namespace ModLoaderLogger
{
	// Fetch the remote manifest, compare build_tag against update_state.ini,
	// and download any outdated or missing plugin DLLs into the Plugins dir.
	// Silent on any network / parse error.  Must be called before LoadAllPlugins().
	void RunAutoUpdate();
}
