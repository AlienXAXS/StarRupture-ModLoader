// Stand-ins for the loader pieces the scanner reaches out to, so sr-aobscan
// runs as a plain console program with no game, no logger and no hooks.
//
// The scan code itself -- memory_scanner/scanner.cpp, scan_validation.cpp,
// image_info.cpp, patch_overlay.cpp -- is compiled from the loader source as it
// is. Only what those call out to is replaced here, and each replacement is
// the "nothing to report" answer:
//   * no logger        -- the verdict comes back in ScanValidation::Result.
//   * no hooks         -- the file on disk is the shipped image, so there is
//                         nothing for PatchOverlay to put back.
//   * no DbgHelp       -- failure reports lose the optional symbol name.
// Three answers are real ones, for the per-build locator selection in
// memory_scanner/version_map.cpp:
//   * the game version -- the mapped exe's own ProductVersion.
//   * "[Map]" lines    -- which locator picked an address goes to stderr with
//                         the rest of the report; every other line is dropped.
//   * the scan cache   -- in memory, for --plugin's second pass. The first pass
//                         runs with nothing cached, as a first launch on a new
//                         exe does, and stores what it verified; the second
//                         reads that back, as the next launch would.
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#pragma comment(lib, "version.lib")

#include "core/version_check.h"
#include "hooks/hook_broker.h"
#include "hooks/symbol_resolver.h"
#include "logging/logger.h"
#include "memory_scanner/scan_cache.h"

namespace
{
	std::wstring g_gameVersion;

	std::map<std::pair<std::wstring, std::string>, ScanCache::Entry> g_cache;
	bool g_cacheWarm = false;

	void PrintMapLine(const wchar_t* format, va_list args)
	{
		if (!format || wcsncmp(format, L"[Map]", 5) != 0)
			return;
		wchar_t line[2048]{};
		_vsnwprintf_s(line, _TRUNCATE, format, args);
		char utf8[4096]{};
		WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
		fprintf(stderr, "%s\n", utf8);
	}
}

namespace ModLoaderLogger
{
	void LogTrace(const wchar_t*, ...) {}
	void LogDebug(const wchar_t*, ...) {}
	void LogInfo(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		PrintMapLine(format, args);
		va_end(args);
	}
	void LogWarn(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		PrintMapLine(format, args);
		va_end(args);
	}
	void LogError(const wchar_t*, ...) {}
}

namespace AobScan
{
	void SetGameExe(const wchar_t* path)
	{
		g_gameVersion.clear();
		DWORD unused = 0;
		const DWORD size = GetFileVersionInfoSizeW(path, &unused);
		if (!size)
			return;
		std::vector<BYTE> info(size);
		if (!GetFileVersionInfoW(path, 0, size, info.data()))
			return;

		struct LangCodepage { WORD lang; WORD codepage; };
		LangCodepage* translations = nullptr;
		UINT bytes = 0;
		wchar_t query[64] = L"\\StringFileInfo\\040904b0\\ProductVersion";
		if (VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations), &bytes)
			&& translations && bytes >= sizeof(LangCodepage))
			swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translations[0].lang, translations[0].codepage);

		wchar_t* version = nullptr;
		UINT length = 0;
		if (VerQueryValueW(info.data(), query, reinterpret_cast<LPVOID*>(&version), &length) && version && length)
			g_gameVersion = version;
	}

	void WarmScanCache()
	{
		g_cacheWarm = true;
	}
}

std::wstring GetGameVersionString()
{
	return g_gameVersion;
}

namespace ScanCache
{
	bool TryGet(const std::wstring& gameVersion, const std::string& pattern, Entry& out)
	{
		if (!g_cacheWarm)
			return false;
		const auto it = g_cache.find({ gameVersion, pattern });
		if (it == g_cache.end())
			return false;
		out = it->second;
		return true;
	}

	void Store(const std::wstring& gameVersion, const std::string& pattern, const Entry& entry)
	{
		g_cache[{ gameVersion, pattern }] = entry;
	}
}

namespace Hooks::Broker
{
	std::vector<PatchedRange> GetPatchedRanges()
	{
		return {};
	}
}

namespace Hooks::SymbolResolver
{
	std::string Resolve(uintptr_t, uintptr_t*)
	{
		return {};
	}
}
