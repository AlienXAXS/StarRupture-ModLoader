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

	// An exe laid out the way the Windows loader lays it out: sections at their
	// RVAs, base relocations applied, .pdata registered. aobscan.cpp.
	struct MappedImage
	{
		uint8_t* base = nullptr;
		size_t   size = 0;
	};
	bool MapImage(const wchar_t* path, MappedImage& out, std::string& error);

	// Makes the exe's ProductVersion the game version the loader code reads
	// (GetGameVersionString). stubs.cpp.
	void SetGameExe(const wchar_t* path);

	namespace Suggest { class Session; }

	// image is the game exe as MapImage laid it out. Returns kExitOk when the
	// loader would let the plugin through to PluginInit, kExitFailed when it
	// would refuse it, kExitBadInput when the DLL is not a loadable client plugin.
	// suggest, when given, proposes a replacement for each MISSING or
	// AMBIGUOUS pattern; it does not change the verdict.
	int ScanPlugin(HMODULE image, const wchar_t* dllPath, bool json, bool verbose, Suggest::Session* suggest);
}
