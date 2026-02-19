#include "scanner.h"
#include "logger.h"
#include <sstream>

std::vector<Scanner::PatternByte> Scanner::ParsePattern(const std::string& pattern)
{
	ModLoader::LogDebug(L"[Scanner] ParsePattern: input = \"%S\"", pattern.c_str());

	std::vector<PatternByte> result;
	std::istringstream stream(pattern);
	std::string token;

	int byteCount = 0;
	int wildcardCount = 0;

	while (stream >> token)
	{
		if (token == "?" || token == "??")
		{
			result.push_back({ 0, true });
			++wildcardCount;
		}
		else
		{
			auto value = static_cast<uint8_t>(std::stoul(token, nullptr, 16));
			result.push_back({ value, false });
			++byteCount;
		}
	}

	ModLoader::LogDebug(L"[Scanner] ParsePattern: %zu total bytes (%d concrete, %d wildcards)",
		result.size(), byteCount, wildcardCount);

	return result;
}

uintptr_t Scanner::FindPattern(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern)
{
	ModLoader::LogTrace(L"[Scanner] FindPattern: scanning 0x%llX -> 0x%llX (%zu bytes, pattern len %zu)",
		static_cast<unsigned long long>(start),
		static_cast<unsigned long long>(start + size),
		size, pattern.size());

	if (pattern.empty())
	{
		ModLoader::LogWarn(L"[Scanner] FindPattern: empty pattern — returning 0");
		return 0;
	}

	if (size < pattern.size())
	{
		ModLoader::LogWarn(L"[Scanner] FindPattern: scan region (%zu bytes) smaller than pattern (%zu bytes) — returning 0",
			size, pattern.size());
		return 0;
	}

	const auto* data = reinterpret_cast<const uint8_t*>(start);
	const size_t scanEnd = size - pattern.size();

	// Progress logging for large scans
	const size_t progressInterval = scanEnd / 10; // Log every ~10%
	size_t nextProgress = progressInterval;
	int percentLogged = 0;

	for (size_t i = 0; i <= scanEnd; ++i)
	{
		// Progress reporting for large scans
		if (progressInterval > 0 && i >= nextProgress)
		{
			percentLogged += 10;
			ModLoader::LogTrace(L"[Scanner] FindPattern: scan progress %d%% (offset 0x%zX / 0x%zX)",
				percentLogged, i, scanEnd);
			nextProgress += progressInterval;
		}

		bool match = true;
		for (size_t j = 0; j < pattern.size(); ++j)
		{
			if (!pattern[j].wildcard && data[i + j] != pattern[j].value)
			{
				match = false;
				break;
			}
		}

		if (match)
		{
			uintptr_t result = start + i;
			ModLoader::LogTrace(L"[Scanner] FindPattern: match at offset 0x%zX (absolute 0x%llX)",
				i, static_cast<unsigned long long>(result));
			return result;
		}
	}

	ModLoader::LogTrace(L"[Scanner] FindPattern: no match after scanning %zu bytes", scanEnd);
	return 0;
}

uintptr_t Scanner::FindPatternInModule(HMODULE module, const std::string& pattern)
{
	ModLoader::LogDebug(L"[Scanner] FindPatternInModule: module handle = 0x%llX",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)));

	if (!module)
	{
		ModLoader::LogError(L"[Scanner] FindPatternInModule: null module handle!");
		return 0;
	}

	// Get module base and size from the PE headers
	auto base = reinterpret_cast<uintptr_t>(module);
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

	ModLoader::LogTrace(L"[Scanner] PE: DOS header e_magic = 0x%04X (expect 0x5A4D)", dos->e_magic);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		ModLoader::LogError(L"[Scanner] FindPatternInModule: invalid DOS signature at 0x%llX!",
			static_cast<unsigned long long>(base));
		return 0;
	}

	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	ModLoader::LogTrace(L"[Scanner] PE: NT headers at offset 0x%lX, signature = 0x%08lX (expect 0x00004550)",
		dos->e_lfanew, nt->Signature);

	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		ModLoader::LogError(L"[Scanner] FindPatternInModule: invalid NT signature!");
		return 0;
	}

	uintptr_t start = base;
	size_t    size = nt->OptionalHeader.SizeOfImage;

	ModLoader::LogInfo(L"[Scanner] Scanning module: base=0x%llX  size=0x%zX (%zu KB)  sections=%u",
		static_cast<unsigned long long>(start), size, size / 1024,
		static_cast<unsigned>(nt->FileHeader.NumberOfSections));
	ModLoader::LogInfo(L"[Scanner]   Pattern: %S", pattern.c_str());

	// Log section layout for debugging
	auto* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		char secName[9]{};
		memcpy(secName, section[i].Name, 8);
		ModLoader::LogDebug(L"[Scanner]   Section [%u]: %-8S  VA=0x%08lX  Size=0x%08lX  Flags=0x%08lX",
			i, secName,
			static_cast<unsigned long>(section[i].VirtualAddress),
			static_cast<unsigned long>(section[i].Misc.VirtualSize),
			static_cast<unsigned long>(section[i].Characteristics));
	}

	auto parsed = ParsePattern(pattern);

	LARGE_INTEGER freqLi, startTime, endTime;
	QueryPerformanceFrequency(&freqLi);
	QueryPerformanceCounter(&startTime);

	uintptr_t result = FindPattern(start, size, parsed);

	QueryPerformanceCounter(&endTime);
	double elapsedMs = static_cast<double>(endTime.QuadPart - startTime.QuadPart) * 1000.0
		/ static_cast<double>(freqLi.QuadPart);

	if (result)
	{
		ModLoader::LogInfo(L"[Scanner]   FOUND at 0x%llX (base+0x%llX) in %.2f ms",
			static_cast<unsigned long long>(result),
			static_cast<unsigned long long>(result - base),
			elapsedMs);
	}
	else
	{
		ModLoader::LogWarn(L"[Scanner]   NOT FOUND (scanned %zu KB in %.2f ms)", size / 1024, elapsedMs);
	}

	return result;
}

uintptr_t Scanner::FindPatternInMainModule(const std::string& pattern)
{
	HMODULE mainModule = GetModuleHandleW(nullptr);
	ModLoader::LogDebug(L"[Scanner] FindPatternInMainModule: main module = 0x%llX",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mainModule)));
	return FindPatternInModule(mainModule, pattern);
}

std::vector<uintptr_t> Scanner::FindAllPatterns(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern)
{
	std::vector<uintptr_t> results;

	ModLoader::LogTrace(L"[Scanner] FindAllPatterns: scanning 0x%llX -> 0x%llX (%zu bytes, pattern len %zu)",
		static_cast<unsigned long long>(start),
		static_cast<unsigned long long>(start + size),
		size, pattern.size());

	if (pattern.empty())
	{
		ModLoader::LogWarn(L"[Scanner] FindAllPatterns: empty pattern");
		return results;
	}

	if (size < pattern.size())
	{
		ModLoader::LogWarn(L"[Scanner] FindAllPatterns: scan region smaller than pattern");
		return results;
	}

	const auto* data = reinterpret_cast<const uint8_t*>(start);
	const size_t scanEnd = size - pattern.size();

	for (size_t i = 0; i <= scanEnd; ++i)
	{
		bool match = true;
		for (size_t j = 0; j < pattern.size(); ++j)
		{
			if (!pattern[j].wildcard && data[i + j] != pattern[j].value)
			{
				match = false;
				break;
			}
		}

		if (match)
		{
			uintptr_t matchAddr = start + i;
			results.push_back(matchAddr);
			ModLoader::LogTrace(L"[Scanner] FindAllPatterns: match #%zu at offset 0x%zX (absolute 0x%llX)",
				results.size(), i, static_cast<unsigned long long>(matchAddr));
		}
	}

	ModLoader::LogDebug(L"[Scanner] FindAllPatterns: found %zu matches", results.size());
	return results;
}

std::vector<uintptr_t> Scanner::FindAllPatternsInModule(HMODULE module, const std::string& pattern)
{
	std::vector<uintptr_t> results;

	ModLoader::LogDebug(L"[Scanner] FindAllPatternsInModule: module handle = 0x%llX",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)));

	if (!module)
	{
		ModLoader::LogError(L"[Scanner] FindAllPatternsInModule: null module handle!");
		return results;
	}

	// Get module base and size
	auto base = reinterpret_cast<uintptr_t>(module);
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		ModLoader::LogError(L"[Scanner] FindAllPatternsInModule: invalid DOS signature!");
		return results;
	}

	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		ModLoader::LogError(L"[Scanner] FindAllPatternsInModule: invalid NT signature!");
		return results;
	}

	uintptr_t start = base;
	size_t size = nt->OptionalHeader.SizeOfImage;

	ModLoader::LogInfo(L"[Scanner] Scanning for ALL matches: base=0x%llX  size=0x%zX",
		static_cast<unsigned long long>(start), size);
	ModLoader::LogInfo(L"[Scanner] Pattern: %S", pattern.c_str());

	auto parsed = ParsePattern(pattern);

	LARGE_INTEGER freqLi, startTime, endTime;
	QueryPerformanceFrequency(&freqLi);
	QueryPerformanceCounter(&startTime);

	results = FindAllPatterns(start, size, parsed);

	QueryPerformanceCounter(&endTime);
	double elapsedMs = static_cast<double>(endTime.QuadPart - startTime.QuadPart) * 1000.0
		/ static_cast<double>(freqLi.QuadPart);

	if (results.empty())
	{
		ModLoader::LogWarn(L"[Scanner]   NO MATCHES found in %.2f ms", elapsedMs);
	}
	else
	{
		ModLoader::LogInfo(L"[Scanner]   Found %zu matches in %.2f ms:", results.size(), elapsedMs);
		
		// Log first 10 matches
		size_t displayCount = (results.size() < 10) ? results.size() : 10;
		for (size_t i = 0; i < displayCount; ++i)
		{
			ModLoader::LogInfo(L"[Scanner]   [%zu] 0x%llX (base+0x%llX)", 
				i,
				static_cast<unsigned long long>(results[i]),
				static_cast<unsigned long long>(results[i] - base));
		}
		
		if (results.size() > displayCount)
		{
			ModLoader::LogInfo(L"[Scanner]   ... and %zu more", results.size() - displayCount);
		}
	}

	return results;
}

std::vector<uintptr_t> Scanner::FindAllPatternsInMainModule(const std::string& pattern)
{
	HMODULE mainModule = GetModuleHandleW(nullptr);
	return FindAllPatternsInModule(mainModule, pattern);
}

uintptr_t Scanner::FindUniquePattern(const std::vector<std::string>& patterns, int* outPatternIndex)
{
	if (patterns.empty())
	{
		ModLoader::LogError(L"[Scanner] FindUniquePattern: no patterns provided");
		return 0;
	}

	HMODULE mainModule = GetModuleHandleW(nullptr);
	auto base = reinterpret_cast<uintptr_t>(mainModule);

	ModLoader::LogInfo(L"[Scanner] FindUniquePattern: trying %zu pattern candidates...", patterns.size());

	for (size_t i = 0; i < patterns.size(); ++i)
	{
		const auto& pattern = patterns[i];
		
		ModLoader::LogDebug(L"[Scanner]   [%zu/%zu] Pattern: %.60S%s", 
			i + 1, patterns.size(), 
			pattern.c_str(),
			pattern.length() > 60 ? "..." : "");

		auto matches = FindAllPatternsInModule(mainModule, pattern);

		if (matches.empty())
		{
			ModLoader::LogDebug(L"[Scanner]     [FAIL] No matches, trying next pattern...");
			continue;
		}
		else if (matches.size() == 1)
		{
			ModLoader::LogInfo(L"[Scanner]     [OK] UNIQUE match found at 0x%llX (base+0x%llX)",
				static_cast<unsigned long long>(matches[0]),
				static_cast<unsigned long long>(matches[0] - base));
			
			if (outPatternIndex)
				*outPatternIndex = static_cast<int>(i);
			
			return matches[0];
		}
		else
		{
			ModLoader::LogWarn(L"[Scanner]     [FAIL] Pattern matched %zu times (not unique)", matches.size());
			
			// Log first few matches for debugging
			size_t displayCount = (matches.size() < 5) ? matches.size() : 5;
			for (size_t j = 0; j < displayCount; ++j)
			{
				ModLoader::LogDebug(L"[Scanner]    Match %zu: 0x%llX", j + 1, 
					static_cast<unsigned long long>(matches[j]));
			}
		}
	}

	ModLoader::LogError(L"[Scanner] FindUniquePattern: no unique pattern found among %zu candidates", patterns.size());
	return 0;
}
