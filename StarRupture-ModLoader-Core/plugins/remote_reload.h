#pragma once

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// RemoteReload
//
// Developer feature, off by default: lets a script or an agent reload a plugin
// without anyone pressing RELOAD. modloader.ini
//
//     [Developer]
//     RemoteReload=1
//
// turns it on. Then a request file dropped into the ModLoader folder
// (remote_reload.txt, one plugin name or DLL file name per line) is picked up
// within about a second, and each name goes through PluginManager::ReloadPlugin
// on the game thread, the same call the Plugins tab's RELOAD button ends in.
// One log line per name says how it went:
//
//     [RemoteReload] <Plugin> reloaded OK v<version>
//     [RemoteReload] <Plugin> reload failed: <reason>
//
// A file, not a pipe: no thread to start and stop, no handle that outlives a
// reload, no access rules to get wrong, and a half-written request is a missing
// file, not a broken connection. The engine tick polls it (PluginManager::
// PumpRemoteReload), at most once a second, with one stat on the game thread.
//
// Header only and free of engine and game state, so the tests build it as is.
// ---------------------------------------------------------------------------
namespace RemoteReload
{
	inline constexpr uint64_t kPollMs   = 1000;   // between two looks for the request file
	inline constexpr size_t   kMaxNames = 16;     // plugins per request; the rest are refused
	inline constexpr size_t   kMaxBytes = 4096;   // request file size read; the rest is ignored

	inline constexpr const wchar_t* kRequestFile = L"remote_reload.txt";
	inline constexpr const wchar_t* kClaimedFile = L"remote_reload.txt.busy";

	// modloader.ini [Developer] RemoteReload, 0 when the file or the key is missing.
	inline bool IsEnabled(const std::wstring& iniPath)
	{
		return GetPrivateProfileIntW(L"Developer", L"RemoteReload", 0, iniPath.c_str()) != 0;
	}

	// True at most once per kPollMs. The first call is due at once.
	class Throttle
	{
	public:
		bool Due(uint64_t nowMs)
		{
			if (m_started && nowMs - m_lastMs < kPollMs)
				return false;
			m_started = true;
			m_lastMs  = nowMs;
			return true;
		}

	private:
		bool     m_started = false;
		uint64_t m_lastMs  = 0;
	};

	struct Request
	{
		std::vector<std::string> names;      // as typed, trimmed, in order, each once
		size_t                   refused = 0; // names past kMaxNames
	};

	inline bool SameName(const std::string& a, const std::string& b)
	{
		return a.size() == b.size() &&
			std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
			{
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
	}

	// One name per line. Blank lines and lines starting with '#' are skipped, a
	// UTF-8 byte order mark and CR are dropped, spaces and tabs around a name
	// are trimmed. A name given twice (any case) is asked once. A line longer
	// than a plugin name can be is kept as it is: the plugin list will not
	// match it and the failure line says so.
	inline Request Parse(const std::string& text)
	{
		Request req;
		size_t pos = 0;
		if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
			pos = 3;

		while (pos <= text.size())
		{
			size_t end = text.find('\n', pos);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(pos, end - pos);
			pos = end + 1;

			const size_t first = line.find_first_not_of(" \t\r");
			if (first == std::string::npos || line[first] == '#')
				continue;
			const size_t last = line.find_last_not_of(" \t\r");
			line = line.substr(first, last - first + 1);

			if (std::any_of(req.names.begin(), req.names.end(), [&](const std::string& n) { return SameName(n, line); }))
				continue;
			if (req.names.size() >= kMaxNames)
			{
				++req.refused;
				continue;
			}
			req.names.push_back(std::move(line));
		}
		return req;
	}

	inline std::string Reloaded(const std::string& plugin, const std::string& version)
	{
		return "[RemoteReload] " + plugin + " reloaded OK v" + version;
	}

	inline std::string Failed(const std::string& plugin, const std::string& why)
	{
		return "[RemoteReload] " + plugin + " reload failed: " + why;
	}

	inline std::string NoSuchPlugin(const std::string& asked)
	{
		return Failed(asked, "no plugin with that name or file name is known to the loader");
	}

	inline std::string TooMany(size_t refused)
	{
		return "[RemoteReload] request ignored for " + std::to_string(refused) + " plugin(s) past the limit of " +
			std::to_string(kMaxNames) + " per request";
	}
}
