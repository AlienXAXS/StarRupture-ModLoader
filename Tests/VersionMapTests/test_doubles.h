#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TestDoubles
{
	// pattern -> RVA in the test exe, for ScanValidation::Resolve.
	extern std::map<std::string, uintptr_t> g_rvas;
	// pattern -> how many places it matches, for a pattern that matches 2 or
	// more (Resolve answers MultipleMatches). One in g_rvas is unique; one in
	// neither matches nowhere.
	extern std::map<std::string, size_t> g_counts;
	// pattern -> how many times Resolve was asked for it.
	extern std::map<std::string, int> g_resolveCalls;
	// Every LogInfo and LogWarn line, formatted.
	extern std::vector<std::wstring> g_log;
	// Every LogDebug line, formatted.
	extern std::vector<std::wstring> g_debugLog;
}
