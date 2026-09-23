#include "memory_scanner/scanner.h"
#include "memory_scanner/scan_cache.h"
#include "memory_scanner/image_info.h"
#include "core/version_check.h"
#include "logging/logger.h"
#include <sstream>
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Scan core.
//
// Two things about the inner loop are worth knowing before changing it.
//
// First, it anchors on the pattern's first concrete (non-wildcard) byte and
// uses memchr to jump between candidates instead of testing every offset. The
// naive byte-by-byte loop this replaced was tolerable only because a resolve
// stopped at its first hit; now that every resolve has to prove a pattern is
// unique (see scan_validation.h and the rules in plugin_interface.h) there is
// no early exit to hide behind, and a full pass over a ~200 MB image happens
// per pattern on a cold cache. memchr is what makes that affordable.
//
// Second, there is deliberately no progress logging inside the loop. The old
// version tested a counter on every byte to emit a trace line every 10%, which
// cost more than the comparison it was reporting on.
// ---------------------------------------------------------------------------

namespace
{
	// Index of the first non-wildcard byte, or SIZE_MAX when the pattern is all
	// wildcards -- which matches at literally every offset and is always an
	// authoring mistake, so callers refuse it rather than reporting millions of
	// matches.
	size_t FindAnchorIndex(const std::vector<Scanner::PatternByte>& pattern)
	{
		for (size_t i = 0; i < pattern.size(); ++i)
			if (!pattern[i].wildcard)
				return i;
		return SIZE_MAX;
	}

	bool MatchesAt(const uint8_t* candidate, const Scanner::PatternByte* pattern, size_t count)
	{
		for (size_t j = 0; j < count; ++j)
			if (!pattern[j].wildcard && candidate[j] != pattern[j].value)
				return false;
		return true;
	}

	// The one scan loop everything else goes through.
	//
	// Calls onMatch for each match in address order and stops early when it
	// returns false. Returns the number of matches reported.
	template <typename OnMatch>
	size_t ScanRange(uintptr_t start, size_t size,
		const std::vector<Scanner::PatternByte>& pattern, OnMatch onMatch)
	{
		const size_t patLen = pattern.size();
		if (patLen == 0 || size < patLen)
			return 0;

		const size_t anchor = FindAnchorIndex(pattern);
		if (anchor == SIZE_MAX)
			return 0;

		const auto*   data       = reinterpret_cast<const uint8_t*>(start);
		const uint8_t anchorByte = pattern[anchor].value;

		// Last offset at which a full pattern still fits, expressed as the
		// position of the anchor byte.
		const uint8_t* const lastAnchor = data + (size - patLen) + anchor;
		const uint8_t*       cursor     = data + anchor;

		size_t found = 0;
		while (cursor <= lastAnchor)
		{
			const auto* hit = static_cast<const uint8_t*>(
				memchr(cursor, anchorByte, static_cast<size_t>(lastAnchor - cursor) + 1));
			if (!hit)
				break;

			const uint8_t* candidate = hit - anchor;
			if (MatchesAt(candidate, pattern.data(), patLen))
			{
				++found;
				if (!onMatch(start + static_cast<size_t>(candidate - data)))
					return found;
			}

			cursor = hit + 1;
		}

		return found;
	}

	// Shared PE validation for the module-scoped entry points.
	bool GetModuleRange(HMODULE module, const wchar_t* caller, uintptr_t& outStart, size_t& outSize)
	{
		if (!module)
		{
			ModLoaderLogger::LogError(L"[Scanner] %s: null module handle!", caller);
			return false;
		}

		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		if (!image.valid)
		{
			ModLoaderLogger::LogError(L"[Scanner] %s: module at 0x%llX has invalid PE headers!",
				caller, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)));
			return false;
		}

		outStart = image.base;
		outSize  = image.size;
		return true;
	}
}

std::vector<Scanner::PatternByte> Scanner::ParsePattern(const std::string& pattern)
{
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
			// A malformed token (anything std::stoul cannot read as hex) used to
			// throw out of the scan and take the init thread with it. Treat it as
			// an empty pattern instead: every caller already handles that as a
			// miss, and the log line names the token that did it.
			try
			{
				auto value = static_cast<uint8_t>(std::stoul(token, nullptr, 16));
				result.push_back({ value, false });
				++byteCount;
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[Scanner] ParsePattern: token '%S' is not a hex byte or wildcard -- pattern rejected: %S",
					token.c_str(), pattern.c_str());
				return {};
			}
		}
	}

	ModLoaderLogger::LogTrace(L"[Scanner] ParsePattern: %zu total bytes (%d concrete, %d wildcards)",
		result.size(), byteCount, wildcardCount);

	return result;
}

uintptr_t Scanner::FindPattern(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern)
{
	if (pattern.empty())
	{
		ModLoaderLogger::LogWarn(L"[Scanner] FindPattern: empty pattern -- returning 0");
		return 0;
	}

	if (size < pattern.size())
		return 0;

	uintptr_t result = 0;
	ScanRange(start, size, pattern, [&result](uintptr_t addr)
	{
		result = addr;
		return false; // first hit is enough
	});

	return result;
}

size_t Scanner::CountMatches(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern,
	size_t stopAfter, uintptr_t* outFirst)
{
	if (outFirst)
		*outFirst = 0;

	if (pattern.empty() || size < pattern.size())
		return 0;

	size_t    count = 0;
	uintptr_t first = 0;

	ScanRange(start, size, pattern, [&](uintptr_t addr)
	{
		if (count == 0)
			first = addr;
		++count;
		return !(stopAfter != 0 && count >= stopAfter);
	});

	if (outFirst)
		*outFirst = first;

	return count;
}

size_t Scanner::CollectMatches(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern,
	size_t maxResults, size_t hardCap, std::vector<uintptr_t>& out)
{
	out.clear();

	if (pattern.empty() || size < pattern.size())
		return 0;

	size_t total = 0;
	ScanRange(start, size, pattern, [&](uintptr_t addr)
	{
		if (out.size() < maxResults)
			out.push_back(addr);
		++total;
		return !(hardCap != 0 && total >= hardCap);
	});

	return total;
}

size_t Scanner::CountMatchesInModule(HMODULE module, const std::string& pattern,
	size_t stopAfter, uintptr_t* outFirst)
{
	uintptr_t start = 0;
	size_t    size  = 0;
	if (!GetModuleRange(module, L"CountMatchesInModule", start, size))
	{
		if (outFirst) *outFirst = 0;
		return 0;
	}

	return CountMatches(start, size, ParsePattern(pattern), stopAfter, outFirst);
}

uintptr_t Scanner::FindPatternInModule(HMODULE module, const std::string& pattern)
{
	uintptr_t start = 0;
	size_t    size  = 0;
	if (!GetModuleRange(module, L"FindPatternInModule", start, size))
		return 0;

	const auto parsed = ParsePattern(pattern);

	LARGE_INTEGER freqLi, startTime, endTime;
	QueryPerformanceFrequency(&freqLi);
	QueryPerformanceCounter(&startTime);

	const uintptr_t result = FindPattern(start, size, parsed);

	QueryPerformanceCounter(&endTime);
	const double elapsedMs = static_cast<double>(endTime.QuadPart - startTime.QuadPart) * 1000.0
		/ static_cast<double>(freqLi.QuadPart);

	if (result)
	{
		ModLoaderLogger::LogDebug(L"[Scanner]   FOUND at 0x%llX (base+0x%llX) in %.2f ms",
			static_cast<unsigned long long>(result),
			static_cast<unsigned long long>(result - start),
			elapsedMs);
	}
	else
	{
		ModLoaderLogger::LogDebug(L"[Scanner]   NOT FOUND (scanned %zu KB in %.2f ms)", size / 1024, elapsedMs);
	}

	return result;
}

uintptr_t Scanner::FindPatternInMainModule(const std::string& patternName, const std::string& pattern)
{
	HMODULE mainModule = GetModuleHandleW(nullptr);
	const auto base = reinterpret_cast<uintptr_t>(mainModule);
	const std::wstring gameVersion = GetGameVersionString();

	// Try the cached offset first -- if the bytes at base+offset still match the
	// pattern, we can skip the full scan entirely. Re-validating against the live
	// process means a stale/corrupt cache entry can never yield a wrong address.
	//
	// The cached match COUNT is deliberately ignored here: this entry point
	// answers "where is it", not "is it unique". The uniqueness verdict belongs
	// to ScanValidation, which has its own cache path -- a caller that needs the
	// verdict must not come through here.
	ScanCache::Entry cached;
	if (!gameVersion.empty() && mainModule && ScanCache::TryGet(gameVersion, pattern, cached))
	{
		const ImageInfo image = GetImageInfo(mainModule);
		if (image.valid)
		{
			const auto parsed = ParsePattern(pattern);
			if (!parsed.empty() && cached.offset + parsed.size() <= image.size)
			{
				const uintptr_t candidate = base + cached.offset;
				if (FindPattern(candidate, parsed.size(), parsed) == candidate)
				{
					ModLoaderLogger::LogDebug(L"[Scanner]   [%S] cache HIT: 0x%llX (base+0x%llX), skipped full scan",
						patternName.c_str(),
						static_cast<unsigned long long>(candidate),
						static_cast<unsigned long long>(cached.offset));
					return candidate;
				}
			}

			ModLoaderLogger::LogDebug(L"[Scanner]   [%S] cache entry stale (offset 0x%llX no longer matches) -- falling back to full scan",
				patternName.c_str(), static_cast<unsigned long long>(cached.offset));
		}
	}

	ModLoaderLogger::LogDebug(L"[Scanner] FindPatternInMainModule: [%S] scanning...", patternName.c_str());

	const uintptr_t result = FindPatternInModule(mainModule, pattern);
	if (result && !gameVersion.empty())
	{
		ScanCache::Entry entry;
		entry.offset     = result - base;
		entry.matchCount = 0; // unknown: this path never counted
		ScanCache::Store(gameVersion, pattern, entry);
	}

	return result;
}

std::vector<uintptr_t> Scanner::FindAllPatterns(uintptr_t start, size_t size, const std::vector<PatternByte>& pattern)
{
	std::vector<uintptr_t> results;

	if (pattern.empty())
	{
		ModLoaderLogger::LogWarn(L"[Scanner] FindAllPatterns: empty pattern");
		return results;
	}

	if (size < pattern.size())
		return results;

	ScanRange(start, size, pattern, [&results](uintptr_t addr)
	{
		results.push_back(addr);
		return true;
	});

	return results;
}

std::vector<uintptr_t> Scanner::FindAllPatternsInModule(HMODULE module, const std::string& pattern)
{
	std::vector<uintptr_t> results;

	uintptr_t start = 0;
	size_t    size  = 0;
	if (!GetModuleRange(module, L"FindAllPatternsInModule", start, size))
		return results;

	const auto parsed = ParsePattern(pattern);

	LARGE_INTEGER freqLi, startTime, endTime;
	QueryPerformanceFrequency(&freqLi);
	QueryPerformanceCounter(&startTime);

	results = FindAllPatterns(start, size, parsed);

	QueryPerformanceCounter(&endTime);
	const double elapsedMs = static_cast<double>(endTime.QuadPart - startTime.QuadPart) * 1000.0
		/ static_cast<double>(freqLi.QuadPart);

	ModLoaderLogger::LogDebug(L"[Scanner] FindAllPatternsInModule: %zu match(es) in %.2f ms  pattern: %S",
		results.size(), elapsedMs, pattern.c_str());

	const size_t displayCount = (results.size() < 10) ? results.size() : 10;
	for (size_t i = 0; i < displayCount; ++i)
	{
		ModLoaderLogger::LogDebug(L"[Scanner]   [%zu] 0x%llX (base+0x%llX)",
			i,
			static_cast<unsigned long long>(results[i]),
			static_cast<unsigned long long>(results[i] - start));
	}
	if (results.size() > displayCount)
		ModLoaderLogger::LogDebug(L"[Scanner]   ... and %zu more", results.size() - displayCount);

	return results;
}

std::vector<uintptr_t> Scanner::FindAllPatternsInMainModule(const std::string& pattern)
{
	return FindAllPatternsInModule(GetModuleHandleW(nullptr), pattern);
}

uintptr_t Scanner::FindUniquePattern(const std::vector<std::string>& patterns, int* outPatternIndex)
{
	if (patterns.empty())
	{
		ModLoaderLogger::LogError(L"[Scanner] FindUniquePattern: no patterns provided");
		return 0;
	}

	HMODULE mainModule = GetModuleHandleW(nullptr);
	const auto base = reinterpret_cast<uintptr_t>(mainModule);

	ModLoaderLogger::LogDebug(L"[Scanner] FindUniquePattern: trying %zu pattern candidates...", patterns.size());

	for (size_t i = 0; i < patterns.size(); ++i)
	{
		// Stop at the second match: the only question is "exactly one or not",
		// and a candidate that has already matched twice is finished.
		uintptr_t first = 0;
		const size_t count = CountMatchesInModule(mainModule, patterns[i], 2, &first);

		if (count == 1)
		{
			ModLoaderLogger::LogDebug(L"[Scanner]   [%zu/%zu] UNIQUE match at 0x%llX (base+0x%llX)",
				i + 1, patterns.size(),
				static_cast<unsigned long long>(first),
				static_cast<unsigned long long>(first - base));

			if (outPatternIndex)
				*outPatternIndex = static_cast<int>(i);

			return first;
		}

		ModLoaderLogger::LogDebug(L"[Scanner]   [%zu/%zu] %s -- trying next candidate",
			i + 1, patterns.size(),
			count == 0 ? L"no matches" : L"matched more than once (not unique)");
	}

	ModLoaderLogger::LogError(L"[Scanner] FindUniquePattern: no unique pattern found among %zu candidates", patterns.size());
	return 0;
}

// ---------------------------------------------------------------------------
// XRef scanning
// ---------------------------------------------------------------------------
//
// Detects two reference types:
//   1. Absolute 8-byte pointer – any 8-byte-aligned location whose value
//      equals targetAddress exactly (vtables, function pointer arrays, etc.)
//   2. Relative near CALL (E8) / JMP (E9) – 4-byte relative offset whose
//      resolved target equals targetAddress.
// ---------------------------------------------------------------------------

std::vector<Scanner::XRef> Scanner::FindXrefsToAddress(uintptr_t targetAddress, uintptr_t start, size_t size)
{
	std::vector<XRef> results;

	if (size < 5)
	{
		ModLoaderLogger::LogWarn(L"[Scanner] FindXrefsToAddress: scan region too small");
		return results;
	}

	ModLoaderLogger::LogInfo(L"[Scanner] FindXrefsToAddress: target=0x%llX  range=0x%llX+0x%zX",
		static_cast<unsigned long long>(targetAddress),
		static_cast<unsigned long long>(start),
		size);

	const auto* data = reinterpret_cast<const uint8_t*>(start);

	// -----------------------------------------------------------------------
	// Pass 1 – absolute 8-byte pointer scan (scans every aligned 8-byte slot)
	// -----------------------------------------------------------------------
	if (size >= sizeof(uintptr_t))
	{
		const size_t ptrEnd = size - sizeof(uintptr_t);
		for (size_t i = 0; i <= ptrEnd; i += sizeof(uintptr_t))
		{
			uintptr_t value;
			memcpy(&value, data + i, sizeof(uintptr_t));
			if (value == targetAddress)
			{
				uintptr_t addr = start + i;
				results.push_back({ addr, false });
				ModLoaderLogger::LogTrace(L"[Scanner] XRef (absolute ptr) at 0x%llX",
					static_cast<unsigned long long>(addr));
			}
		}
	}

	// -----------------------------------------------------------------------
	// Pass 2 – relative near CALL (E8) and near JMP (E9)
	// -----------------------------------------------------------------------
	const size_t relEnd = size - 5; // need 5 bytes: opcode + 4-byte offset
	for (size_t i = 0; i <= relEnd; ++i)
	{
		const uint8_t opcode = data[i];
		if (opcode == 0xE8 || opcode == 0xE9)
		{
			int32_t rel32;
			memcpy(&rel32, data + i + 1, sizeof(int32_t));

			// Computed target: address of next instruction (i+5) + signed offset
			uintptr_t instrAddr = start + i;
			uintptr_t computedTarget = instrAddr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));

			if (computedTarget == targetAddress)
			{
				results.push_back({ instrAddr, true });
				ModLoaderLogger::LogTrace(L"[Scanner] XRef (rel %s) at 0x%llX  rel32=0x%08X",
					opcode == 0xE8 ? L"CALL" : L"JMP",
					static_cast<unsigned long long>(instrAddr),
					static_cast<unsigned int>(static_cast<uint32_t>(rel32)));
			}
		}
	}

	ModLoaderLogger::LogInfo(L"[Scanner] FindXrefsToAddress: found %zu xrefs (%zu absolute, %zu relative)",
		results.size(),
		static_cast<size_t>(std::count_if(results.begin(), results.end(), [](const XRef& x) { return !x.isRelative; })),
		static_cast<size_t>(std::count_if(results.begin(), results.end(), [](const XRef& x) { return  x.isRelative; })));

	return results;
}

std::vector<Scanner::XRef> Scanner::FindXrefsToAddressInModule(uintptr_t targetAddress, HMODULE module)
{
	std::vector<XRef> results;

	if (!module)
	{
		ModLoaderLogger::LogError(L"[Scanner] FindXrefsToAddressInModule: null module handle!");
		return results;
	}

	auto base = reinterpret_cast<uintptr_t>(module);
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		ModLoaderLogger::LogError(L"[Scanner] FindXrefsToAddressInModule: invalid DOS signature!");
		return results;
	}

	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		ModLoaderLogger::LogError(L"[Scanner] FindXrefsToAddressInModule: invalid NT signature!");
		return results;
	}

	size_t imageSize = nt->OptionalHeader.SizeOfImage;

	ModLoaderLogger::LogInfo(L"[Scanner] FindXrefsToAddressInModule: module=0x%llX  size=0x%zX  target=0x%llX",
		static_cast<unsigned long long>(base),
		imageSize,
		static_cast<unsigned long long>(targetAddress));

	LARGE_INTEGER freqLi, startTime, endTime;
	QueryPerformanceFrequency(&freqLi);
	QueryPerformanceCounter(&startTime);

	results = FindXrefsToAddress(targetAddress, base, imageSize);

	QueryPerformanceCounter(&endTime);
	double elapsedMs = static_cast<double>(endTime.QuadPart - startTime.QuadPart) * 1000.0
		/ static_cast<double>(freqLi.QuadPart);

	ModLoaderLogger::LogInfo(L"[Scanner] FindXrefsToAddressInModule: %zu xrefs found in %.2f ms",
		results.size(), elapsedMs);

	// Log up to 10 results
	size_t displayCount = (results.size() < 10) ? results.size() : 10;
	for (size_t i = 0; i < displayCount; ++i)
	{
		ModLoaderLogger::LogInfo(L"[Scanner]   [%zu] 0x%llX (base+0x%llX)  %s",
			i,
			static_cast<unsigned long long>(results[i].address),
			static_cast<unsigned long long>(results[i].address - base),
			results[i].isRelative ? L"relative call/jmp" : L"absolute pointer");
	}
	if (results.size() > displayCount)
		ModLoaderLogger::LogInfo(L"[Scanner]   ... and %zu more", results.size() - displayCount);

	return results;
}

std::vector<Scanner::XRef> Scanner::FindXrefsToAddressInMainModule(uintptr_t targetAddress)
{
	HMODULE mainModule = GetModuleHandleW(nullptr);
	ModLoaderLogger::LogDebug(L"[Scanner] FindXrefsToAddressInMainModule: target=0x%llX  module=0x%llX",
		static_cast<unsigned long long>(targetAddress),
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mainModule)));
	return FindXrefsToAddressInModule(targetAddress, mainModule);
}
