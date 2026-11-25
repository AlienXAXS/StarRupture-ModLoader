#pragma once

// The Vortex mod manager deploys a mod's files as links and records each one,
// with its modified time, in vortex.deployment*.json in the folder it deployed
// to (the game's Win64 directory for the loader and its plugins). When
// something else replaces one of those files, Vortex asks on its next deploy
// which copy to keep. These checks tell the updater and the loader that an
// update replaced such a file, so they can say which answer keeps the update.
// Everything here only reads, and anything that cannot be read or parsed counts
// as "not managed" (Deployment answers Unsure instead). Header-only: the
// loader and the updater exe both use it.

#include "json_extract.h"
#include "../utils/engine_call_utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

namespace VortexNotice
{
	inline constexpr const char* kMessage =
		"Vortex manages these files. When Vortex shows External Changes, choose "
		"\"Save change (use deployed file)\" for each updated mod, then Confirm. "
		"Revert would undo this update.";

	// A manifest lists every file of every mod deployed to the folder, so it can
	// be large; one past this is not read.
	inline constexpr long long kMaxManifestBytes = 16 * 1024 * 1024;

	// Vortex records the modified time in milliseconds but reads it in whole
	// seconds. An update lands long after the deploy it replaces.
	inline constexpr long long kTimeToleranceMs = 5000;

	// A path as it is compared: lower case, backslashes.
	inline std::string NormalizePath(std::string path)
	{
		for (char& c : path)
			c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
		return path;
	}

	// A file's modified time in milliseconds since 1970, or -1 when it cannot
	// be read. Also -1 for a link (a reparse point): the time read would be the
	// link's own. A file an update wrote is a regular file.
	inline long long ModifiedTimeMs(const std::wstring& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
		    (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			return -1;
		const long long ticks =
			(static_cast<long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
			data.ftLastWriteTime.dwLowDateTime; // 100 ns since 1601
		return ticks / 10000 - 11644473600000LL;
	}

	// The whole manifest, or empty when it is missing, too large or not a
	// complete JSON object (a truncated one).
	inline std::string ReadManifest(const std::wstring& path)
	{
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return {};

		std::string text;
		LARGE_INTEGER size{};
		if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxManifestBytes)
		{
			text.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			if (!ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size())
				text.clear();
		}
		CloseHandle(file);

		const size_t first = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
		if (first == std::string::npos || text[first] != '{')
			return {};

		// The object has to close where the text ends. A cut-short manifest can
		// still end in the brace of its last complete entry.
		const std::vector<std::string> object = JsonSplitObjectArray("[" + text.substr(first), 0, 1);
		const size_t end = text.find_last_not_of(" \t\r\n") + 1;
		if (object.empty() || object[0].size() != end - first)
			return {};
		return text;
	}

	// The file a "files" entry records, relative to the deploy folder, as it is
	// compared. relPath is under `target`, a folder in the deploy folder that is
	// empty for the loader's own mod types.
	inline std::string ListedPath(const std::string& entry)
	{
		const std::string target = JsonExtractString(entry, "target");
		const std::string relPath = JsonExtractString(entry, "relPath");
		return NormalizePath(target.empty() ? relPath : target + "\\" + relPath);
	}

	// True when `manifest` lists one of `paths` (UTF-8, relative to gameDir) with a
	// modified time other than the one the file on disk has now.
	inline bool ManifestShowsReplaced(const std::string& manifest, const std::wstring& gameDir,
	                                  const std::vector<std::string>& paths)
	{
		for (const std::string& entry : JsonExtractObjectArray(manifest, "files"))
		{
			const std::string listed = ListedPath(entry);
			for (const std::string& path : paths)
			{
				if (listed != NormalizePath(path))
					continue;

				// Milliseconds since 1970: past what JsonExtractInt holds.
				const size_t value = JsonFindValue(entry, "time");
				if (value == std::string::npos || !isdigit(static_cast<unsigned char>(entry[value])))
					continue;
				const long long recorded = std::strtoll(entry.c_str() + value, nullptr, 10);

				const long long onDisk = ModifiedTimeMs(gameDir + L"\\" + EngineCall::Widen(path));
				if (onDisk >= 0 && std::llabs(onDisk - recorded) > kTimeToleranceMs)
					return true;
			}
		}
		return false;
	}

	enum class Listing
	{
		NotListed,
		Listed,
		Unsure, // not in any manifest that could be read, and one could not
	};

	// What the deploy folder's manifests list, read once so that any number of
	// files can be asked about.
	struct Deployment
	{
		std::unordered_set<std::string> files; // ListedPath of every entry
		bool unreadable = false;               // a manifest could not be read

		// Did Vortex place one of `paths` (relative to the deploy folder, either
		// slash, UTF-8)? A manifest that cannot be read does not count as "no":
		// the answer is then Unsure, unless another manifest lists the path.
		Listing Lists(const std::vector<std::string>& paths) const
		{
			for (const std::string& path : paths)
				if (files.count(NormalizePath(path)))
					return Listing::Listed;
			return unreadable ? Listing::Unsure : Listing::NotListed;
		}
	};

	inline Deployment ReadDeployment(const std::wstring& gameDir)
	{
		Deployment out;
		WIN32_FIND_DATAW found{};
		HANDLE search = FindFirstFileW((gameDir + L"\vortex.deployment*.json").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
		{
			out.unreadable = GetLastError() != ERROR_FILE_NOT_FOUND;
			return out;
		}
		do
		{
			const std::string manifest = ReadManifest(gameDir + L"\\" + found.cFileName);
			if (manifest.empty())
				out.unreadable = true;
			else
				for (const std::string& entry : JsonExtractObjectArray(manifest, "files"))
					out.files.insert(ListedPath(entry));
		} while (FindNextFileW(search, &found));
		FindClose(search);
		return out;
	}

	// Did replacing `paths` (relative to gameDir, either slash) replace a file
	// Vortex placed? Call it after the files are in place.
	inline bool ReplacedVortexFiles(const std::wstring& gameDir, const std::vector<std::string>& paths)
	{
		if (paths.empty())
			return false;

		WIN32_FIND_DATAW found{};
		HANDLE search = FindFirstFileW((gameDir + L"\\vortex.deployment*.json").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE)
			return false;

		bool replaced = false;
		do
		{
			replaced = ManifestShowsReplaced(ReadManifest(gameDir + L"\\" + found.cFileName), gameDir, paths);
		} while (!replaced && FindNextFileW(search, &found));
		FindClose(search);
		return replaced;
	}
}
