#pragma once

#include "auto_update/json_extract.h"
#include "plugins/plugin_manage.h"

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
//       "interface_version_min": 79, "tested_loader": "1.27.0",
//       "readme_url": "https://raw.githubusercontent.com/...",
//       "requires": ["minimap"], "server_only": true }, ... ]
// id, name and page_url are required; the rest may be left out. server_only
// marks a plugin that runs on a dedicated server (the Browse tab labels it). The file is
// read as untrusted text: sizes are capped, control characters become spaces,
// and an entry whose page_url is not on the allowlist is dropped, because the
// Browse tab opens that address in the player's browser. A readme_url that is
// not on its allowlist is dropped from the entry, not the entry. requires is
// the ids of the plugins this one needs: strings only, never objects.
//
// An entry the Browse tab may install also has download_url (the .dll asset of a
// GitHub release of page_url's repository), download_sha256 (64 lowercase hex),
// download_size (bytes), install_files (the .dll name to write and, for
// auto-updates, the same name with .json) and, exactly when the .json is listed,
// update_manifest_url. One of them missing or wrong: the entry is still listed,
// with nothing to install.
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
	constexpr size_t kMaxTestedLoaderLen = 32;
	constexpr size_t kMaxSummaryLen = 300;
	constexpr size_t kMaxUrlLen     = 200;   // longer: the entry is dropped, never cut
	constexpr size_t kMaxRequires   = 8;     // each cut at kMaxIdLen, as an id is
	constexpr int    kMaxDownloadBytes = 32 * 1024 * 1024;

	// What installing an entry downloads and writes. dllFile empty: nothing.
	struct InstallInfo
	{
		std::string url;
		std::string sha256;
		size_t size = 0;
		std::string dllFile;      // a bare ASCII plugin DLL name
		std::string manifestUrl;  // empty: no .json is written beside the DLL
	};

	struct Entry
	{
		InstallInfo install;
		std::string id;
		std::string name;
		std::string author;
		std::string version;
		std::string summary;
		std::string pageUrl;                 // always passes IsAllowedPageUrl
		std::string testedLoader;            // the ModLoader version the entry was last tried on; empty: not stated
		std::string readmeUrl;               // empty, or passes IsAllowedReadmeUrl
		int interfaceVersionMin = -1;        // -1: not stated
		bool serverOnly = false;             // "server_only": true; runs on a dedicated server
		std::vector<std::string> requiredIds; // "requires": ids of the plugins this one needs
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

		// True only for a literal true; absent or anything else reads as false.
		inline bool ReadBool(const std::string& json, const char* key)
		{
			const size_t at = JsonFindValue(json, key);
			return at != std::string::npos && json.compare(at, 4, "true") == 0;
		}

		inline int ReadInt(const std::string& json, const char* key)
		{
			try { return JsonExtractInt(json, key, -1); }
			catch (...) { return -1; }    // a number too large for an int reads as absent
		}
	}

	namespace detail
	{
		// The shared URL check: https, at most kMaxUrlLen bytes, no control or
		// non-ASCII character, no space, no backslash, no user info or port, and
		// a host that equals one of `hosts` (ignoring case).
		template <size_t N>
		inline bool IsAllowedUrl(const std::string& url, const char* const (&hosts)[N])
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

			for (const char* allowed : hosts)
				if (EqualsNoCase(host, allowed))
					return true;
			return false;
		}
	}

	// Only https, on these hosts exactly: nexusmods.com, www.nexusmods.com,
	// github.com, www.github.com. No user info, no port, no backslash, no
	// control or non-ASCII character, no space.
	inline bool IsAllowedPageUrl(const std::string& url)
	{
		static const char* const kHosts[] = { "nexusmods.com", "www.nexusmods.com", "github.com", "www.github.com" };
		return detail::IsAllowedUrl(url, kHosts);
	}

	// A link in a README the player may click: a page address, or the same rules
	// on youtube.com, www.youtube.com, m.youtube.com or youtu.be. A mod's index
	// entry keeps to IsAllowedPageUrl.
	inline bool IsAllowedLinkUrl(const std::string& url)
	{
		static const char* const kHosts[] = { "youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be" };
		return IsAllowedPageUrl(url) || detail::IsAllowedUrl(url, kHosts);
	}

	// A plugin's README: the same rules, on the host raw.githubusercontent.com only.
	inline bool IsAllowedReadmeUrl(const std::string& url)
	{
		static const char* const kHosts[] = { "raw.githubusercontent.com" };
		return detail::IsAllowedUrl(url, kHosts);
	}

	namespace detail
	{
		// ASCII letters, digits, '_', '.', '-', with no "..": nothing a URL
		// reads differently (no '%', '?', '#') and nothing a path does.
		inline bool IsPlainSegment(const std::string& s)
		{
			if (s.empty() || s == "." || s.find("..") != std::string::npos)
				return false;
			for (char c : s)
				if (!((Lower(c) >= 'a' && Lower(c) <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
					return false;
			return true;
		}

		inline bool EndsWithNoCase(const std::string& s, const std::string& tail)
		{
			return s.size() >= tail.size() && EqualsNoCase(s.substr(s.size() - tail.size()), tail);
		}

		// A device name stays one with any extension.
		inline bool IsDeviceName(const std::string& file)
		{
			std::string base = file.substr(0, file.find('.'));
			for (char& c : base)
				c = Lower(c);
			if (base == "con" || base == "prn" || base == "aux" || base == "nul")
				return true;
			return base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0)
				&& base[3] >= '0' && base[3] <= '9';
		}

		// A file name the index may have the loader write or download as a plugin.
		inline bool IsInstallDllName(const std::string& name)
		{
			return name.size() <= 80 && IsPlainSegment(name) && name.front() != '.' && !IsDeviceName(name)
				&& !EndsWithNoCase(name, "-Server.dll")
				&& PluginManage::IsPluginDllName(std::wstring(name.begin(), name.end()));
		}

		// Every piece of a URL split on '/', empty ones included, so the count is exact.
		inline std::vector<std::string> SplitSegments(const std::string& s)
		{
			std::vector<std::string> parts;
			size_t start = 0;
			for (;;)
			{
				const size_t slash = s.find('/', start);
				parts.push_back(s.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
				if (slash == std::string::npos)
					return parts;
				start = slash + 1;
			}
		}
	}

	// The only address a download may start at:
	// https://github.com/<owner>/<repo>/releases/download/<tag>/<name>.dll
	inline bool IsReleaseAssetUrl(const std::string& url)
	{
		static const std::string kStart = "https://github.com/";
		if (url.size() > kMaxUrlLen || url.compare(0, kStart.size(), kStart) != 0)
			return false;
		const std::vector<std::string> p = detail::SplitSegments(url.substr(kStart.size()));
		if (p.size() != 6 || p[2] != "releases" || p[3] != "download")
			return false;
		for (const std::string& part : p)
			if (!detail::IsPlainSegment(part))
				return false;
		return detail::IsInstallDllName(p[5]);
	}

	namespace detail
	{
		// The entry's install fields, or none when any of them is missing or wrong.
		// The download and the manifest must belong to page_url's repository.
		inline InstallInfo ReadInstall(const std::string& obj, const std::string& pageUrl)
		{
			InstallInfo info;
			info.url    = JsonExtractDecodedString(obj, "download_url");
			info.sha256 = JsonExtractDecodedString(obj, "download_sha256");
			const int size = ReadInt(obj, "download_size");
			if (!IsReleaseAssetUrl(info.url) || info.sha256.size() != 64
				|| info.sha256.find_first_not_of("0123456789abcdef") != std::string::npos
				|| size < 1 || size > kMaxDownloadBytes)
				return {};
			info.size = static_cast<size_t>(size);

			const std::vector<std::string> page = SplitSegments(pageUrl);   // https: "" host owner repo ...
			const std::vector<std::string> from = SplitSegments(info.url);
			if (page.size() < 5 || !EqualsNoCase(page[2], "github.com")
				|| !EqualsNoCase(page[3], from[3]) || !EqualsNoCase(page[4], from[4]))
				return {};

			const std::vector<std::string> files = JsonExtractStringArray(obj, "install_files");
			if (files.empty() || files.size() > 2)
				return {};
			std::string json;
			for (const std::string& file : files)
			{
				if (IsInstallDllName(file) && info.dllFile.empty())
					info.dllFile = file;
				else if (json.empty() && EndsWithNoCase(file, ".json"))
					json = file;
				else
					return {};
			}
			if (info.dllFile.empty() || (!json.empty() && !EqualsNoCase(json, info.dllFile.substr(0, info.dllFile.size() - 4) + ".json")))
				return {};

			info.manifestUrl = JsonExtractDecodedString(obj, "update_manifest_url");
			if (info.manifestUrl.empty() != json.empty())
				return {};
			if (!json.empty())
			{
				const std::string start = "https://github.com/" + from[3] + "/" + from[4] + "/releases/latest/download/";
				if (info.manifestUrl.size() > kMaxUrlLen || info.manifestUrl.size() <= start.size()
					|| !EqualsNoCase(info.manifestUrl.substr(0, start.size()), start))
					return {};
				const std::string name = info.manifestUrl.substr(start.size());
				if (!IsPlainSegment(name) || !EndsWithNoCase(name, "-client-manifest.json"))
					return {};
			}
			return info;
		}
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
			e.testedLoader = detail::Clean(JsonExtractDecodedString(obj, "tested_loader"), kMaxTestedLoaderLen);
			const std::string readme = JsonExtractDecodedString(obj, "readme_url");
			if (IsAllowedReadmeUrl(readme))
				e.readmeUrl = readme;
			e.serverOnly = detail::ReadBool(obj, "server_only");
			e.interfaceVersionMin = detail::ReadInt(obj, "interface_version_min");
			if (e.interfaceVersionMin < 0)
				e.interfaceVersionMin = -1;
			for (const std::string& item : JsonExtractStringArray(obj, "requires"))
			{
				std::string id = detail::Clean(item, kMaxIdLen);
				if (!id.empty() && e.requiredIds.size() < kMaxRequires)
					e.requiredIds.push_back(std::move(id));
			}
			// A server plugin is never installed on a PC, whatever else the entry says.
			if (!e.serverOnly)
				e.install = detail::ReadInstall(obj, e.pageUrl);
			result.entries.push_back(std::move(e));
		}

		result.ok = true;
		return result;
	}

	// The Browse tab's search box. The words of the search, lowercased; none:
	// show everything.
	inline std::vector<std::string> SearchTokens(const std::string& query)
	{
		std::vector<std::string> tokens;
		std::string word;
		for (char c : query)
		{
			if (c == ' ' || c == '	')
			{
				if (!word.empty())
					tokens.push_back(std::move(word));
				word.clear();
			}
			else
				word.push_back(detail::Lower(c));
		}
		if (!word.empty())
			tokens.push_back(std::move(word));
		return tokens;
	}

	namespace detail
	{
		// `word` (already lowercase) appears in `text`, ignoring case.
		inline bool ContainsWord(const std::string& text, const std::string& word)
		{
			if (word.size() > text.size())
				return false;
			for (size_t i = 0; i + word.size() <= text.size(); ++i)
			{
				size_t j = 0;
				while (j < word.size() && Lower(text[i + j]) == word[j])
					++j;
				if (j == word.size())
					return true;
			}
			return false;
		}

		// The letters of `word` (already lowercase) appear in `text` in order,
		// with anything between them, ignoring case: "minmap" in "MiniMap".
		inline bool InOrder(const std::string& text, const std::string& word)
		{
			size_t at = 0;
			for (char c : text)
				if (at < word.size() && Lower(c) == word[at])
					++at;
			return at == word.size();
		}
	}

	// Whether a listing passes the search: every word has to match, in any
	// order. A word matches when it is inside the name, the author or the
	// description, or, from 3 letters up, when its letters follow each other in
	// the name. The loose match stays off the description, which is long enough
	// to hold any short word's letters by chance.
	inline bool MatchesSearch(const std::vector<std::string>& tokens, const std::string& name,
	                          const std::string& author, const std::string& description)
	{
		for (const std::string& word : tokens)
		{
			const bool found = detail::ContainsWord(name, word) || detail::ContainsWord(author, word) ||
			                   detail::ContainsWord(description, word) ||
			                   (word.size() >= 3 && detail::InOrder(name, word));
			if (!found)
				return false;
		}
		return true;
	}

	inline bool MatchesSearch(const std::vector<std::string>& tokens, const Entry& entry)
	{
		return MatchesSearch(tokens, entry.name, entry.author, entry.summary);
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
