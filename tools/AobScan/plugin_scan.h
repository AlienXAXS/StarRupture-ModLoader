#pragma once
// sr-aobscan --plugin: runs a plugin's own OnPluginLoadHooks against the mapped
// game image, through the loader's own IPluginHookScanner, and reports every
// pattern it resolved. See plugin_scan.cpp.
#include <cstdint>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace AobScan
{
	// Exit codes shared by both modes.
	constexpr int kExitOk       = 0;
	constexpr int kExitFailed   = 1;
	constexpr int kExitBadInput = 2;

	std::string JsonEscape(const std::string& s);

	// image is the game exe as MapImage laid it out. Returns kExitOk when the
	// loader would let the plugin through to PluginInit, kExitFailed when it
	// would refuse it, kExitBadInput when the DLL is not a loadable client plugin.
	int ScanPlugin(HMODULE image, const wchar_t* dllPath, bool json, bool verbose);
}
