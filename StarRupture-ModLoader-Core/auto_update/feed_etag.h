#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// The ETag a fetched file's cache came with, kept beside the cache file as
// "<seq> <etag>" (feed_fetch.cpp, mod_index_signed.cpp).
namespace FeedFetch
{
	// The ETag of the answer the cache holds, kept next to it as "<seq> <etag>".
	inline std::wstring EtagPath(const std::wstring& cachePath)
	{
		return cachePath + L".etag";
	}

	// The stored ETag, only when it belongs to the cache's seq and is plain text.
	inline std::string ReadEtag(const std::wstring& cachePath, uint64_t cacheSeq)
	{
		std::string text;
		HANDLE h = CreateFileW(EtagPath(cachePath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return {};
		char buf[256];
		DWORD got = 0;
		if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr))
			text.assign(buf, got);
		CloseHandle(h);

		char* end = nullptr;
		const unsigned long long seq = strtoull(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != ' ' || seq != cacheSeq)
			return {};
		std::string etag(end + 1);
		for (char c : etag)
			if (c < 0x20 || c > 0x7E)
				return {};
		return etag;
	}

	// Best effort: with no ETag on file the next start asks for the whole feed.
	inline void WriteEtag(const std::wstring& cachePath, uint64_t seq, const std::string& etag)
	{
		HANDLE h = CreateFileW(EtagPath(cachePath).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		const std::string text = std::to_string(seq) + " " + etag;
		DWORD wrote = 0;
		WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
		CloseHandle(h);
	}
}
