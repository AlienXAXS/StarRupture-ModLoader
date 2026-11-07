// Test doubles for the scanner, the scan cache, the game version and the log,
// so memory_scanner/version_map.cpp runs without a game.
#include "test_doubles.h"

#include "core/version_check.h"
#include "logging/logger.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_cache.h"
#include "memory_scanner/scan_validation.h"
#include "memory_scanner/scanner.h"

#include <cstdarg>
#include <cwchar>

namespace TestDoubles
{
	std::map<std::string, uintptr_t> g_rvas;
	std::map<std::string, size_t>    g_counts;
	std::map<std::string, int>       g_resolveCalls;
	std::vector<std::wstring>        g_log;
	std::vector<std::wstring>        g_debugLog;
}

namespace
{
	void Capture(std::vector<std::wstring>& into, const wchar_t* format, va_list args)
	{
		wchar_t line[2048]{};
		vswprintf_s(line, format, args);
		into.emplace_back(line);
	}
}

namespace ModLoaderLogger
{
	void LogInfo(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		Capture(TestDoubles::g_log, format, args);
		va_end(args);
	}

	void LogWarn(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		Capture(TestDoubles::g_log, format, args);
		va_end(args);
	}

	void LogError(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		Capture(TestDoubles::g_log, format, args);
		va_end(args);
	}

	// scanner.cpp's trace lines: not what the tests read.
	void LogTrace(const wchar_t*, ...) {}
	void LogDebug(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		Capture(TestDoubles::g_debugLog, format, args);
		va_end(args);
	}
}

namespace ScanValidation
{
	// Found when the pattern, as given, is in the table; the address is the
	// test exe's base plus the table's RVA.
	Result Resolve(const Request& request)
	{
		Result r;
		++TestDoubles::g_resolveCalls[request.pattern];
		const auto many = TestDoubles::g_counts.find(request.pattern);
		if (many != TestDoubles::g_counts.end())
		{
			r.outcome    = Outcome::MultipleMatches;
			r.matchCount = many->second;
			r.detail     = "pattern is not unique";
			return r;
		}
		const auto it = TestDoubles::g_rvas.find(request.pattern);
		if (it == TestDoubles::g_rvas.end())
		{
			r.outcome = Outcome::NoMatch;
			r.detail  = "pattern not found";
			return r;
		}
		r.outcome    = Outcome::Ok;
		r.matchCount = 1;
		r.rawMatch   = Scanner::GetImageInfo(GetModuleHandleW(nullptr)).base + it->second;
		r.address    = r.rawMatch + request.resultOffset;
		return r;
	}

	Result CheckAddress(const Request&, uintptr_t address)
	{
		Result r;
		r.outcome    = Outcome::Ok;
		r.address    = address;
		r.rawMatch   = address;
		r.matchCount = 1;
		return r;
	}

	const char* KindName(Kind kind)
	{
		switch (kind)
		{
		case Kind::FunctionStart: return "function start";
		case Kind::InFunction:    return "in function";
		case Kind::Data:          return "data";
		default:                  return "other";
		}
	}
}

namespace ScanCache
{
	bool TryGet(const std::wstring&, const std::string&, Entry&) { return false; }
	void Store(const std::wstring&, const std::string&, const Entry&) {}
}

std::wstring GetGameVersionString() { return std::wstring(); }
