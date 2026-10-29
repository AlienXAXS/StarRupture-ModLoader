// Stand-ins for the loader pieces the scanner reaches out to, so sr-aobscan
// runs as a plain console program with no game, no logger and no hooks.
//
// The scan code itself -- memory_scanner/scanner.cpp, scan_validation.cpp,
// image_info.cpp, patch_overlay.cpp -- is compiled from the loader source as it
// is. Only what those call out to is replaced here, and each replacement is
// the "nothing to report" answer:
//   * no logger        -- the verdict comes back in ScanValidation::Result.
//   * no scan cache    -- an offline scan always reads the image.
//   * no hooks         -- the file on disk is the shipped image, so there is
//                         nothing for PatchOverlay to put back.
//   * no DbgHelp       -- failure reports lose the optional symbol name.
#include <string>
#include <vector>

#include "core/version_check.h"
#include "hooks/hook_broker.h"
#include "hooks/symbol_resolver.h"
#include "logging/logger.h"
#include "memory_scanner/scan_cache.h"

namespace ModLoaderLogger
{
	void LogTrace(const wchar_t*, ...) {}
	void LogDebug(const wchar_t*, ...) {}
	void LogInfo(const wchar_t*, ...) {}
	void LogWarn(const wchar_t*, ...) {}
	void LogError(const wchar_t*, ...) {}
}

std::wstring GetGameVersionString()
{
	return {};
}

namespace ScanCache
{
	bool TryGet(const std::wstring&, const std::string&, Entry&)
	{
		return false;
	}

	void Store(const std::wstring&, const std::string&, const Entry&) {}
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
