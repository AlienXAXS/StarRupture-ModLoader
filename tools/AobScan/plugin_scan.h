#pragma once
// sr-aobscan --plugin: runs a plugin's own OnPluginLoadHooks against the mapped
// game image, through the loader's own IPluginHookScanner, and reports every
// pattern it resolved. See plugin_scan.cpp.
#include <cstdint>
#include <cstdio>
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

	// A failure detail as ScanValidation writes it: lines separated by "\r\n",
	// continuation lines indented. PrintDetail prints each line under its row;
	// JsonDetail gives the lines without the indent, joined with "\n".
	void        PrintDetail(FILE* out, const std::string& detail);
	std::string JsonDetail(const std::string& detail);

	// image is the game exe as MapImage laid it out. Returns kExitOk when the
	// loader would let the plugin through to PluginInit, kExitFailed when it
	// would refuse it, kExitBadInput when the DLL is not a loadable client plugin.
	int ScanPlugin(HMODULE image, const wchar_t* dllPath, bool json, bool verbose);
}
