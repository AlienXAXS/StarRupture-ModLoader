#pragma once

#include "plugins/plugin_interface.h"
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Hooks::HttpServer
//
// Modloader-owned hook on FHttpConnection::ProcessRequest.
//
// URL scheme (always case-insensitive):
//   /<pluginName>/<routeName>/...
//
// Detour pipeline (in order):
//   1. Raw-request filters  — RegisterRawRequestFilter / UnregisterRawRequestFilter
//     First Deny sends 403 and stops all further processing.
//   2. Raw-response routes  — AddRawRoute / RemoveRawRoute
// Plugin-owned handler; plugin writes status, content-type, and body.
//   3. Static-file routes   — AddRoute / RemoveRoute
//Files served from <exe_dir>\Plugins\<pluginName>\<folderName>\.
//   4. Pass-through         — the original engine handler is called.
//
// This subsystem is only installed on server builds.
// hooks->HttpServer is nullptr on client and generic builds.
// ---------------------------------------------------------------------------

namespace Hooks::HttpServer
{
	// -----------------------------------------------------------------------
	// Lifecycle — called by the modloader (dllmain / hooks_interface)
	// -----------------------------------------------------------------------

	bool Install();
	void Remove();
	bool IsInstalled();

	// -----------------------------------------------------------------------
	// Static-file routes
	// URL matched: /<pluginName>/<folderName>/...  (case-insensitive)
	// Files served from: <exe_dir>\Plugins\<pluginName>\<folderName>\
	// -----------------------------------------------------------------------

	bool AddRoute(const char* pluginName, const char* folderName);
	void RemoveRoute(const char* pluginName, const char* folderName);

	// -----------------------------------------------------------------------
	// Raw-request filters
	// -----------------------------------------------------------------------

	void RegisterRawRequestFilter(PluginHttpRequestFilterCallback callback);
	void UnregisterRawRequestFilter(PluginHttpRequestFilterCallback callback);

	// -----------------------------------------------------------------------
	// Raw-response routes
	// URL matched: /<pluginName>/<urlPrefix>/...  (case-insensitive)
	// -----------------------------------------------------------------------

	bool AddRawRoute(const char* pluginName, const char* urlPrefix, PluginHttpRouteCallback callback);
	void RemoveRawRoute(const char* pluginName, const char* urlPrefix);

} // namespace Hooks::HttpServer
