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

	// What a plugin's own pattern earns by its match count on the game exe: a
	// request that resolved must have a pattern that matches exactly once, or
	// the version map is carrying a pattern the loader would refuse without it.
	// nullptr when it matches once. plugin_scan.cpp, GradeOwnPatterns.
	inline const char* OwnPatternLabel(size_t matches)
	{
		return matches == 1 ? nullptr : matches == 0 ? "OWN-MISSING" : "OWN-AMBIGUOUS";
	}

	// Whether a row is graded on its own pattern (the first): a recorded request
	// that resolved and has one. A request with locators for other builds
	// (Resolve) counts; a call that lists alternatives (the Unique calls) does
	// not, since only one of them has to match. A request with no pattern (a map
	// key alone) has no own pattern to grade.
	inline bool OwnPatternGraded(const std::string& call, bool recorded, const std::string& result,
		size_t patternCount, bool firstPatternEmpty)
	{
		return recorded && result == "OK" && patternCount > 0 && !firstPatternEmpty
			&& (call == "Resolve" || patternCount == 1);
	}

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

	// From here on the scan cache answers with what was stored before, as on
	// the next launch of the same exe. stubs.cpp.
	void WarmScanCache();

	namespace Suggest { class Session; }
	class SymbolStarts;

	// image is the game exe as MapImage laid it out. Returns kExitOk when the
	// loader would let the plugin through to PluginInit, kExitFailed when it
	// would refuse it, kExitBadInput when the DLL is not a loadable client plugin.
	// suggest, when given, proposes a replacement for each MISSING or
	// AMBIGUOUS pattern; it does not change the verdict. symbols gives each OK
	// row's symbolRva in the JSON.
	int ScanPlugin(HMODULE image, const wchar_t* dllPath, bool json, bool verbose, Suggest::Session* suggest,
		SymbolStarts& symbols);
}
