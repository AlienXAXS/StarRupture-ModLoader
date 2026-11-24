#pragma once

#include "auto_update/json_extract.h"

#include <cstddef>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// mod_index.h -- the curated list of ModLoader plugins that the overlay's
// Browse tab shows. Header only, so a test can use it without the loader.
//
// The file is one JSON array of objects:
//   [ { "id": "codex", "name": "Codex", "author": "...", "version": "1.2.3",
//       "summary": "...", "page_url": "https://github.com/...",
//       "interface_version_min": 79 }, ... ]
// id, name and page_url are required; the rest may be left out. The file is
// read as untrusted text: sizes are capped, control characters become spaces,
// and an entry whose page_url is not on the allowlist is dropped, because the
// Browse tab opens that address in the player's browser.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	// Limits. A longer index fails; a longer field is cut; a longer list is cut.
	constexpr size_t kMaxIndexBytes = 256 * 1024;
	constexpr size_t kMaxEntries    = 200;
	constexpr size_t kMaxIdLen      = 64;
	constexpr size_t kMaxNameLen    = 64;
	constexpr size_t kMaxAuthorLen  = 64;
	constexpr size_t kMaxVersionLen = 32;
	constexpr size_t kMaxSummaryLen = 300;
	constexpr size_t kMaxUrlLen     = 200;   // longer: the entry is dropped, never cut

	struct Entry
	{
		std::string id;
		std::string name;
		std::string author;
		std::string version;
		std::string summary;
		std::string pageUrl;                 // always passes IsAllowedPageUrl
		int interfaceVersionMin = -1;        // -1: not stated
	};

	struct ParseResult
	{
		bool ok = false;
		std::vector<Entry> entries;
		int  skipped   = 0;                  // objects dropped: a required field missing, or a page_url not allowed
		bool truncated = false;              // more than kMaxEntries objects; the rest were ignored
		std::string error;                   // plain words, set when !ok
	};

	namespace detail
	{
		inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

		inline bool EqualsNoCase(const std::string& a, const std::string& b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (Lower(a[i]) != Lower(b[i]))
					return false;
			return true;
		}

		// Control characters (and NUL) become spaces, the ends are trimmed, and the
		// text is cut at maxLen bytes without ending inside a UTF-8 sequence.
		inline std::string Clean(std::string s, size_t maxLen)
		{
			for (char& c : s)
				if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
					c = ' ';
			const size_t first = s.find_first_not_of(' ');
			if (first == std::string::npos)
				return {};
			s = s.substr(first, s.find_last_not_of(' ') - first + 1);
			if (s.size() > maxLen)
			{
				size_t cut = maxLen;
				while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
					--cut;
				s.resize(cut);
			}
			return s;
		}

		inline int ReadInt(const std::string& json, const char* key)
		{
			try { return JsonExtractInt(json, key, -1); }
			catch (...) { return -1; }    // a number too large for an int reads as absent
		}
	}

	// Only https, on these hosts exactly: nexusmods.com, www.nexusmods.com,
	// github.com, www.github.com. No user info, no port, no backslash, no
	// control or non-ASCII character, no space.
	inline bool IsAllowedPageUrl(const std::string& url)
	{
		static const char kScheme[] = "https://";
		constexpr size_t kSchemeLen = sizeof(kScheme) - 1;
		if (url.size() <= kSchemeLen || url.size() > kMaxUrlLen || url.compare(0, kSchemeLen, kScheme) != 0)
			return false;

		for (char c : url)
		{
			const unsigned char u = static_cast<unsigned char>(c);
			if (u <= 0x20 || u >= 0x7F || c == '\\')
				return false;
		}

		const size_t end = url.find_first_of("/?#", kSchemeLen);
		const std::string host = url.substr(kSchemeLen, end == std::string::npos ? std::string::npos : end - kSchemeLen);
		if (host.find_first_of("@:") != std::string::npos)
			return false;

		static const char* const kHosts[] = { "nexusmods.com", "www.nexusmods.com", "github.com", "www.github.com" };
		for (const char* allowed : kHosts)
			if (detail::EqualsNoCase(host, allowed))
				return true;
		return false;
	}

	// Parse the index text. One bad object never fails the list: it is skipped
	// and counted. The text itself failing (too large, not an array, cut off)
	// returns ok = false with a plain message.
	inline ParseResult ParseModIndex(const std::string& text)
	{
		ParseResult result;

		if (text.size() > kMaxIndexBytes)
		{
			result.error = "The mod list is too large to read.";
			return result;
		}

		size_t first = 0;
		if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
			first = 3;
		const char* ws = " \t\r\n";
		first = text.find_first_not_of(ws, first);
		const size_t last = text.find_last_not_of(ws);
		if (first == std::string::npos || text[first] != '[')
		{
			result.error = "The mod list is not a JSON array.";
			return result;
		}
		if (text[last] != ']')
		{
			result.error = "The mod list is cut off or not valid JSON.";
			return result;
		}

		const std::vector<std::string> objects = JsonSplitObjectArray(text, first, kMaxEntries + 1);
		result.truncated = objects.size() > kMaxEntries;

		const size_t count = result.truncated ? kMaxEntries : objects.size();
		for (size_t i = 0; i < count; ++i)
		{
			const std::string& obj = objects[i];
			Entry e;
			e.id      = detail::Clean(JsonExtractDecodedString(obj, "id"),      kMaxIdLen);
			e.name    = detail::Clean(JsonExtractDecodedString(obj, "name"),    kMaxNameLen);
			e.pageUrl = JsonExtractDecodedString(obj, "page_url");
			if (e.id.empty() || e.name.empty() || !IsAllowedPageUrl(e.pageUrl))
			{
				++result.skipped;
				continue;
			}
			e.author  = detail::Clean(JsonExtractDecodedString(obj, "author"),  kMaxAuthorLen);
			e.version = detail::Clean(JsonExtractDecodedString(obj, "version"), kMaxVersionLen);
			e.summary = detail::Clean(JsonExtractDecodedString(obj, "summary"), kMaxSummaryLen);
			e.interfaceVersionMin = detail::ReadInt(obj, "interface_version_min");
			if (e.interfaceVersionMin < 0)
				e.interfaceVersionMin = -1;
			result.entries.push_back(std::move(e));
		}

		result.ok = true;
		return result;
	}

	// Whether a plugin with this entry's id or name is loaded. Case-insensitive.
	// loadedPluginNames: the names the loader shows for its loaded plugins.
	inline bool IsInstalled(const Entry& entry, const std::vector<std::string>& loadedPluginNames)
	{
		for (const std::string& loaded : loadedPluginNames)
			if (!loaded.empty() && (detail::EqualsNoCase(loaded, entry.id) || detail::EqualsNoCase(loaded, entry.name)))
				return true;
		return false;
	}
}
