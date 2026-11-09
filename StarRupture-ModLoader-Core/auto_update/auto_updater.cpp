// auto_updater.cpp
// Plugin auto-update system — see auto_updater.h for overview.

#include "auto_updater.h"
#include "http_get.h"
#include "plugin_dll_version.h"
#include "version_compare.h"
#include "feed_fetch.h"
#include "core/startup_utils.h"
#include "logging/log.h"
#include "plugins/plugin_interface.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/global_settings.h"
#include "UI/splash_window.h"
#include "UI/update_notice_window.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// ===========================================================================
// Section A — Configuration
// ===========================================================================

struct AutoUpdateConfig
{
	bool enabled = true;
	char manifestUrl[512] = {};
	bool urlFromIni = false; // true if user overrode the URL in modloader.ini
};

static AutoUpdateConfig ReadAutoUpdateConfig()
{
	AutoUpdateConfig cfg;

	wchar_t iniPath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
	wchar_t* slash = wcsrchr(iniPath, L'\\');
	if (slash)
		wcscpy_s(slash + 1, static_cast<rsize_t>(MAX_PATH - (slash + 1 - iniPath)), L"ModLoader\\modloader.ini");

	LogToFile::Debug("[AutoUpdate] Reading config from: %ls", iniPath);

	cfg.enabled = (GetPrivateProfileIntW(L"AutoUpdate", L"Enabled", 1, iniPath) != 0);
	LogToFile::Debug("[AutoUpdate] Enabled=%s (from ini)", cfg.enabled ? "true" : "false");

	wchar_t wUrl[512]{};
	GetPrivateProfileStringW(L"AutoUpdate", L"ManifestUrl", L"", wUrl, 512, iniPath);

	if (wUrl[0] == L'\0')
	{
		// Fall back to the compiled-in default (set by CI)
		strncpy_s(cfg.manifestUrl, sizeof(cfg.manifestUrl),
		          AUTOUPDATE_DEFAULT_MANIFEST_URL, _TRUNCATE);
		cfg.urlFromIni = false;
		LogToFile::Debug("[AutoUpdate] ManifestUrl: using compiled-in default");
	}
	else
	{
		WideCharToMultiByte(CP_UTF8, 0, wUrl, -1,
		                    cfg.manifestUrl, sizeof(cfg.manifestUrl),
		                    nullptr, nullptr);
		cfg.urlFromIni = true;
		LogToFile::Debug("[AutoUpdate] ManifestUrl: overridden via modloader.ini");
	}

	return cfg;
}

// ===========================================================================
// Section B — Update state (ModLoader\update_state.ini)
// ===========================================================================

static void GetUpdateStateIniPath(wchar_t* outPath, DWORD maxLen)
{
	GetModuleFileNameW(nullptr, outPath, maxLen);
	wchar_t* slash = wcsrchr(outPath, L'\\');
	if (slash)
		wcscpy_s(slash + 1,
		         maxLen - static_cast<DWORD>(slash + 1 - outPath),
		         L"ModLoader\\update_state.ini");
}

static void ReadStoredBuildTag(char* outTag, int maxLen)
{
	wchar_t iniPath[MAX_PATH]{};
	GetUpdateStateIniPath(iniPath, MAX_PATH);

	wchar_t wTag[256]{};
	GetPrivateProfileStringW(L"AutoUpdate", L"BuildTag", L"", wTag, 256, iniPath);

	WideCharToMultiByte(CP_UTF8, 0, wTag, -1, outTag, maxLen, nullptr, nullptr);
}

static void WriteStoredBuildTag(const char* tag)
{
	wchar_t iniPath[MAX_PATH]{};
	GetUpdateStateIniPath(iniPath, MAX_PATH);

	wchar_t wTag[256]{};
	MultiByteToWideChar(CP_UTF8, 0, tag, -1, wTag, 256);

	if (WritePrivateProfileStringW(L"AutoUpdate", L"BuildTag", wTag, iniPath))
		LogToFile::Debug("[AutoUpdate] Stored build tag written: %s", tag);
	else
	{
		DWORD err = GetLastError();
		LogToFile::Warn("[AutoUpdate] Failed to write build tag to update_state.ini (%lu: %s)", err,
		                FormatWinHttpError(err).c_str());
	}
}

// The release the loader last listed its notes for (the manifest's build_tag).
static void ReadLoaderNotesShown(char* outTag, int maxLen)
{
	wchar_t iniPath[MAX_PATH]{};
	GetUpdateStateIniPath(iniPath, MAX_PATH);

	wchar_t wTag[256]{};
	GetPrivateProfileStringW(L"AutoUpdate", L"LoaderNotesShown", L"", wTag, 256, iniPath);

	WideCharToMultiByte(CP_UTF8, 0, wTag, -1, outTag, maxLen, nullptr, nullptr);
}

// The version the player had before the notes still waiting to be shown, kept
// from the launch that listed them until the window opens (see
// ListLoaderNotesOnce). Empty value: listed, previous version unknown.
static void ReadLoaderNotesPrevious(char* outTag, int maxLen)
{
	wchar_t iniPath[MAX_PATH]{};
	GetUpdateStateIniPath(iniPath, MAX_PATH);

	wchar_t wTag[256]{};
	GetPrivateProfileStringW(L"AutoUpdate", L"LoaderNotesPrevious", L"", wTag, 256, iniPath);

	WideCharToMultiByte(CP_UTF8, 0, wTag, -1, outTag, maxLen, nullptr, nullptr);
}

// value == nullptr removes the key.
static bool WriteLoaderNotesKey(const wchar_t* key, const char* value)
{
	wchar_t iniPath[MAX_PATH]{};
	GetUpdateStateIniPath(iniPath, MAX_PATH);

	wchar_t wValue[256]{};
	if (value)
		MultiByteToWideChar(CP_UTF8, 0, value, -1, wValue, 256);

	if (WritePrivateProfileStringW(L"AutoUpdate", key, value ? wValue : nullptr, iniPath))
		return true;

	DWORD err = GetLastError();
	LogToFile::Warn("[AutoUpdate] Failed to write %ls to update_state.ini (%lu: %s)", key, err,
	                FormatWinHttpError(err).c_str());
	return false;
}

// The updater's log of the current boot and of the previous one
// (ModLoader\Logs\AutoUpdate.log and the newest AutoUpdate-<time>.log beside
// it). They say which version an update came from; see ResolvePreviousVersion.
// The tail is enough: the lines wanted are written on every boot, and a log
// is a few KB.
static std::string ReadLogTail(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return {};

	constexpr DWORD kMaxBytes = 256 * 1024;
	LARGE_INTEGER size{};
	std::string text;
	if (GetFileSizeEx(h, &size))
	{
		LARGE_INTEGER from{};
		from.QuadPart = size.QuadPart > kMaxBytes ? size.QuadPart - kMaxBytes : 0;
		if (SetFilePointerEx(h, from, nullptr, FILE_BEGIN))
		{
			text.resize(kMaxBytes);
			DWORD got = 0;
			if (!ReadFile(h, text.data(), kMaxBytes, &got, nullptr))
				got = 0;
			text.resize(got);
		}
	}
	CloseHandle(h);
	return text;
}

static void ReadUpdaterLogs(std::string& currentLog, std::string& previousLog)
{
	wchar_t statePath[MAX_PATH]{};
	GetUpdateStateIniPath(statePath, MAX_PATH);
	std::wstring logsDir = statePath;
	logsDir.resize(logsDir.find_last_of(L'\\'));
	logsDir += L"\\Logs";

	currentLog = ReadLogTail(logsDir + L"\\AutoUpdate.log");

	// The archives are named by time, so the last name is the newest.
	std::wstring newest;
	WIN32_FIND_DATAW fd{};
	HANDLE find = FindFirstFileW((logsDir + L"\\AutoUpdate-*.log").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && newest < fd.cFileName)
				newest = fd.cFileName;
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	previousLog = newest.empty() ? std::string() : ReadLogTail(logsDir + L"\\" + newest);
}

// ===========================================================================
// Section D — Minimal JSON helpers
// No external library — only handles the flat manifest schema.
// ===========================================================================

// Extract the value of a top-level string field: "key": "value"
// Handles basic \" escapes inside the value.
static std::string JsonExtractString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return {};

	size_t vstart = json.find('"', kpos + needle.size());
	if (vstart == std::string::npos)
		return {};
	++vstart;

	std::string value;
	for (size_t i = vstart; i < json.size(); ++i)
	{
		if (json[i] == '\\' && i + 1 < json.size())
		{
			value += json[i + 1];
			++i;
		}
		else if (json[i] == '"')
		{
			break;
		}
		else
		{
			value += json[i];
		}
	}
	return value;
}

// Extract a top-level string field and decode its JSON escapes. Used for the
// manifest's "notes": JsonExtractString above drops the backslash, so it
// cannot tell \n from the letter n. Handles \n \t \" \\ \/ and \uXXXX; \r is
// dropped, and a \u escape outside printable ASCII becomes '?' so the text
// always draws in the overlay font.
static std::string JsonExtractDecodedString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return {};

	size_t vstart = json.find('"', kpos + needle.size());
	if (vstart == std::string::npos)
		return {};
	++vstart;

	std::string value;
	for (size_t i = vstart; i < json.size(); ++i)
	{
		if (json[i] == '"')
			break;

		if (json[i] != '\\' || i + 1 >= json.size())
		{
			value += json[i];
			continue;
		}

		switch (json[++i])
		{
		case 'n': value += '\n'; break;
		case 't': value += '\t'; break;
		case 'r': break;
		case 'u':
		{
			bool ok = i + 4 < json.size();
			for (size_t k = 1; ok && k <= 4; ++k)
				ok = isxdigit(static_cast<unsigned char>(json[i + k])) != 0;
			const unsigned code = ok ? std::stoul(json.substr(i + 1, 4), nullptr, 16) : 0;
			if (ok)
				i += 4;
			value += (ok && (code == '\n' || (code >= 0x20 && code < 0x7F))) ? static_cast<char>(code) : '?';
			break;
		}
		default: value += json[i]; break;
		}
	}
	return value;
}

// Extract the value of a top-level integer field: "key": 12
static int JsonExtractInt(const std::string& json, const char* key, int fallback = 0)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return fallback;

	size_t vpos = kpos + needle.size();
	while (vpos < json.size() && (json[vpos] == ' ' || json[vpos] == '\t'
		|| json[vpos] == '\r' || json[vpos] == '\n'
		|| json[vpos] == ':'))
		++vpos;

	if (vpos >= json.size() || !isdigit(static_cast<unsigned char>(json[vpos])))
		return fallback;

	return std::stoi(json.substr(vpos));
}

// Split the "plugins": [ ... ] array into individual { } object strings.
// Works via brace-depth tracking; assumes no nested objects inside entries.
static std::vector<std::string> JsonExtractObjectArray(
	const std::string& json, const char* arrayKey)
{
	std::vector<std::string> results;

	std::string needle = std::string("\"") + arrayKey + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return results;

	size_t apos = json.find('[', kpos + needle.size());
	if (apos == std::string::npos)
		return results;

	size_t i = apos + 1;
	while (i < json.size())
	{
		if (json[i] == ' ' || json[i] == '\t' ||
			json[i] == '\r' || json[i] == '\n' || json[i] == ',')
		{
			++i;
			continue;
		}

		if (json[i] == ']')
			break;

		if (json[i] == '{')
		{
			size_t start = i;
			int depth = 0;
			bool inString = false;
			while (i < json.size())
			{
				char c = json[i];
				if (inString)
				{
					if (c == '\\') { ++i; }
					else if (c == '"') { inString = false; }
				}
				else
				{
					if (c == '"') { inString = true; }
					else if (c == '{') { ++depth; }
					else if (c == '}')
					{
						--depth;
						if (depth == 0)
						{
							results.push_back(json.substr(start, i - start + 1));
							++i;
							break;
						}
					}
				}
				++i;
			}
			continue;
		}

		++i;
	}

	return results;
}

// ===========================================================================
// Section E — Download a single plugin DLL
// ===========================================================================

// Downloads url into <pluginsDir>\<filename>.tmp then atomically renames to
// the final path.  Returns true on success; the original DLL is never removed
// unless the new file is fully written and placed.
//
// Note on shape: a download-to-temp-then-replace-a-DLL routine matches the
// generic "dropper" pattern that AV/EDR heuristics watch for, so it is
// documented here rather than left to speak for itself:
//   - Only reachable from RunAutoUpdate(), which only runs when a manifest
//     URL is configured (CI-injected AUTOUPDATE_MANIFEST_URL on release
//     builds, or an explicit opt-in via modloader.ini ManifestUrl) — see
//     auto_updater.h. Dev/generic builds have no URL and never call this.
//   - url always comes from a manifest the user/CI already chose to trust
//     (the central release manifest, or a plugin's own sidecar-declared
//     manifest_url) — this function does not discover or follow untrusted
//     links on its own.
//   - The downloaded bytes only ever land at <pluginsDir>\<filename>, i.e.
//     they replace a plugin DLL the user already installed; this code never
//     writes outside the Plugins directory and never executes anything
//     directly — the replaced DLL is picked up by the normal LoadLibrary
//     path on next plugin load.
//   - There is no payload checksum/signature check beyond what HTTPS (via
//     WinHttp) already guarantees for transport integrity; trust is anchored
//     at the manifest URL, not at this function.
static bool DownloadPlugin(const char* url,
                           const wchar_t* pluginsDir,
                           const wchar_t* filename,
                           const char* displayName)
{
	wchar_t tmpPath[MAX_PATH]{};
	wchar_t finalPath[MAX_PATH]{};
	swprintf_s(tmpPath, L"%s\\%s.tmp", pluginsDir, filename);
	swprintf_s(finalPath, L"%s\\%s", pluginsDir, filename);

	std::string body = HttpGet(url, displayName);
	if (body.empty())
	{
		LogToFile::Debug("[AutoUpdate] Download [%s]: HttpGet returned empty body", displayName);
		return false;
	}

	LogToFile::Debug("[AutoUpdate] Download [%s]: writing %zu bytes to temp file", displayName, body.size());

	HANDLE hFile = CreateFileW(tmpPath, GENERIC_WRITE, 0, nullptr,
	                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] Download [%s]: CreateFileW failed for temp path (%lu: %s): %ls",
		                 displayName, err, FormatWinHttpError(err).c_str(), tmpPath);
		return false;
	}

	DWORD written = 0;
	BOOL ok = WriteFile(hFile, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
	DWORD writeErr = GetLastError();
	CloseHandle(hFile);

	if (!ok || written != static_cast<DWORD>(body.size()))
	{
		LogToFile::Debug("[AutoUpdate] Download [%s]: WriteFile incomplete — wrote %lu of %zu bytes (error %lu)",
		                 displayName, written, body.size(), writeErr);
		DeleteFileW(tmpPath);
		return false;
	}

	LogToFile::Debug("[AutoUpdate] Download [%s]: %lu bytes written, renaming to final path", displayName, written);

	// Atomic rename: replace existing in one operation so the original is
	// never removed unless the new file is successfully placed.
	if (!MoveFileExW(tmpPath, finalPath, MOVEFILE_REPLACE_EXISTING))
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] Download [%s]: MoveFileExW failed (%lu: %s) — temp file removed",
		                 displayName, err, FormatWinHttpError(err).c_str());
		DeleteFileW(tmpPath);
		return false;
	}

	LogToFile::Debug("[AutoUpdate] Download [%s]: placed at %ls", displayName, finalPath);
	return true;
}

// ===========================================================================
// Section G — Per-plugin sidecar manifest updates
//
// Each plugin DLL may ship a companion sidecar file with the same base name
// and a .json extension, e.g. Plugins\ServerUtility.json.  The sidecar
// contains a single "manifest_url" field pointing to the plugin author's own
// update manifest.  This lets third-party plugins self-host their updates
// independently of the central modloader release cycle.
//
// Sidecar format (Plugins\MyPlugin.json):
//   {
//     "manifest_url": "https://example.com/myplugin/manifest.json"
//   }
//
// Remote per-plugin manifest format:
//   {
//     "plugin_name":           "MyPlugin",
//     "version":               "1.2.0",
//     "interface_version_min": 19,
//     "interface_version_max": 22,
//     "download_url":          "https://example.com/releases/MyPlugin-1.2.0.dll",
//     "notes":                 "Fixed a crash on load\nAdded a new option"
//   }
//
// "notes" is optional: one change per line, lines separated by the \n escape.
// When present it is listed under the plugin in the "Plugins Updated" pop-up.
//
// The installed version per plugin is stored in update_state.ini under
// [PluginVersions], keyed by the DLL filename (e.g. "MyPlugin.dll=1.2.0"): the
// version last downloaded, raised to the one the installed DLL reports for
// itself if that is newer, so a build installed by hand is known. The pass
// reads the DLL before it compares (PluginDllVersion::Read), because it runs
// before the plugins load; NoteInstalledPluginVersion does the same raise once
// a plugin has loaded, for a DLL that could not be read. Only a remote version
// newer than that replaces the DLL, using the same atomic .tmp -> rename
// pattern as the central updater. With no version known at all, nothing is
// downloaded until the plugin has loaded once.
// ===========================================================================

struct PluginSidecar
{
    std::string dllFilename;  // e.g. "MyPlugin.dll"
    std::string manifestUrl;  // value of "manifest_url" in the sidecar JSON
};

// Read the entire content of a file on disk into a std::string.
// Returns an empty string on any error.
static std::string ReadFileToString(const wchar_t* path)
{
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart == 0 || size.QuadPart > (1 << 20))
    {
        CloseHandle(hFile);
        return {};
    }

    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr);
    CloseHandle(hFile);

    if (!ok || bytesRead != static_cast<DWORD>(buf.size()))
        return {};

    return buf;
}

// Read "manifest_url" from a sidecar JSON file on disk.
// Returns empty string on any error (file missing, field absent, etc.).
static std::string ReadSidecarManifestUrl(const wchar_t* jsonPath)
{
    std::string content = ReadFileToString(jsonPath);
    if (content.empty())
        return {};
    return JsonExtractString(content, "manifest_url");
}

// Scan the Plugins directory for *.json files that have a matching *.dll.
// Returns one PluginSidecar per valid pairing.
static std::vector<PluginSidecar> ScanForSidecars(const wchar_t* pluginsDir)
{
    std::vector<PluginSidecar> results;

    wchar_t pattern[MAX_PATH]{};
    swprintf_s(pattern, L"%s\\*.json", pluginsDir);

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return results;

    do
    {
        // Skip directories
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        // Build full path to the .json sidecar
        wchar_t jsonPath[MAX_PATH]{};
        swprintf_s(jsonPath, L"%s\\%s", pluginsDir, fd.cFileName);

        // Derive the expected DLL name by replacing .json extension with .dll
        wchar_t dllName[MAX_PATH]{};
        wcscpy_s(dllName, fd.cFileName);
        wchar_t* dot = wcsrchr(dllName, L'.');
        if (!dot)
            continue;
        wcscpy_s(dot, static_cast<rsize_t>(MAX_PATH - (dot - dllName)), L".dll");

        // Only process if a matching DLL actually exists on disk
        wchar_t dllPath[MAX_PATH]{};
        swprintf_s(dllPath, L"%s\\%s", pluginsDir, dllName);
        if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
        {
            // Sidecar present but DLL absent — plugin was removed, skip silently
            char narrowName[256]{};
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, narrowName, sizeof(narrowName), nullptr, nullptr);
            LogToFile::Debug("[AutoUpdate][Sidecar] '%s' has no matching DLL — skipping", narrowName);
            continue;
        }

        std::string manifestUrl = ReadSidecarManifestUrl(jsonPath);
        if (manifestUrl.empty())
        {
            char narrowName[256]{};
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, narrowName, sizeof(narrowName), nullptr, nullptr);
            LogToFile::Warn("[AutoUpdate][Sidecar] '%s' is missing 'manifest_url' — skipping", narrowName);
            continue;
        }

        char narrowDll[256]{};
        WideCharToMultiByte(CP_UTF8, 0, dllName, -1, narrowDll, sizeof(narrowDll), nullptr, nullptr);

        PluginSidecar sc;
        sc.dllFilename  = narrowDll;
        sc.manifestUrl  = manifestUrl;
        results.push_back(std::move(sc));

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return results;
}

// Read the stored version for a plugin from update_state.ini [PluginVersions].
// Returns empty string if not found.
static std::string ReadPluginVersion(const wchar_t* iniPath, const char* dllFilename)
{
    wchar_t wKey[256]{};
    MultiByteToWideChar(CP_UTF8, 0, dllFilename, -1, wKey, 256);

    wchar_t wVal[256]{};
    GetPrivateProfileStringW(L"PluginVersions", wKey, L"", wVal, 256, iniPath);

    char val[256]{};
    WideCharToMultiByte(CP_UTF8, 0, wVal, -1, val, sizeof(val), nullptr, nullptr);
    return val;
}

// Write the stored version for a plugin to update_state.ini [PluginVersions].
static void WritePluginVersion(const wchar_t* iniPath,
                                const char* dllFilename,
                                const char* version)
{
    wchar_t wKey[256]{}, wVal[256]{};
    MultiByteToWideChar(CP_UTF8, 0, dllFilename, -1, wKey, 256);
    MultiByteToWideChar(CP_UTF8, 0, version,     -1, wVal, 256);

    if (WritePrivateProfileStringW(L"PluginVersions", wKey, wVal, iniPath))
        LogToFile::Debug("[AutoUpdate][Sidecar] Stored version for '%s': %s", dllFilename, version);
    else
    {
        DWORD err = GetLastError();
        LogToFile::Warn("[AutoUpdate][Sidecar] Failed to write version for '%s' (%lu: %s)",
                        dllFilename, err, FormatWinHttpError(err).c_str());
    }
}

void NoteInstalledPluginVersion(const wchar_t* dllFileName, const char* version)
{
    if (!dllFileName || !version)
        return;

    wchar_t stateIniPath[MAX_PATH]{};
    GetUpdateStateIniPath(stateIniPath, MAX_PATH);
    char key[256]{};
    WideCharToMultiByte(CP_UTF8, 0, dllFileName, -1, key, sizeof(key), nullptr, nullptr);

    const std::string stored = ReadPluginVersion(stateIniPath, key);
    if (AutoUpdateVersion::InstalledReplacesStored(version, stored.c_str()))
        WritePluginVersion(stateIniPath, key, version);
}

// Fetch and process a single per-plugin manifest.
// Downloads the plugin DLL if the remote version is newer than stored.
static void ProcessPluginSidecar(const PluginSidecar& sc,
                                  const wchar_t* pluginsDir,
                                  const wchar_t* stateIniPath,
                                  int index, int total)
{
    LogToFile::Debug("[AutoUpdate][Sidecar] Checking '%s' via %s",
                     sc.dllFilename.c_str(), sc.manifestUrl.c_str());

    std::string json = HttpGet(sc.manifestUrl.c_str(), sc.dllFilename.c_str());
    if (json.empty())
    {
        LogToFile::Warn("[AutoUpdate][Sidecar] '%s' — manifest fetch failed, skipping",
                        sc.dllFilename.c_str());
        return;
    }

    std::string remoteVersion  = JsonExtractString(json, "version");
    std::string downloadUrl    = JsonExtractString(json, "download_url");
    std::string pluginName     = JsonExtractString(json, "plugin_name");
    std::string notes          = JsonExtractDecodedString(json, "notes");
    std::string releaseNotes   = JsonExtractDecodedString(json, "release_notes");
    int ifaceMin = JsonExtractInt(json, "interface_version_min", -1);
    int ifaceMax = JsonExtractInt(json, "interface_version_max", -1);

    const char* displayName = pluginName.empty() ? sc.dllFilename.c_str() : pluginName.c_str();

    if (remoteVersion.empty())
    {
        LogToFile::Warn("[AutoUpdate][Sidecar] '%s' — manifest missing 'version' field, skipping",
                        displayName);
        return;
    }

    if (downloadUrl.empty())
    {
        LogToFile::Warn("[AutoUpdate][Sidecar] '%s' — manifest missing 'download_url' field, skipping",
                        displayName);
        return;
    }

    // Interface version gate — skip if the plugin declares a range that does
    // not overlap with what this loader supports.
    if (ifaceMin != -1 && ifaceMax != -1)
    {
        if (ifaceMax < PLUGIN_INTERFACE_VERSION_MIN ||
            ifaceMin > PLUGIN_INTERFACE_VERSION_MAX)
        {
            LogToFile::Warn("[AutoUpdate][Sidecar] '%s' v%s requires interface [%d, %d], "
                            "loader supports [%d, %d] — skipping",
                            displayName, remoteVersion.c_str(),
                            ifaceMin, ifaceMax,
                            PLUGIN_INTERFACE_VERSION_MIN, PLUGIN_INTERFACE_VERSION_MAX);
            return;
        }
    }

    wchar_t wFilename[256]{};
    MultiByteToWideChar(CP_UTF8, 0, sc.dllFilename.c_str(), -1, wFilename, 256);

    // The plugins have not loaded yet, so the DLL itself says what is
    // installed. A build put in by hand can be newer than the version stored
    // here, and no release older than it may replace it.
    wchar_t dllPath[MAX_PATH]{};
    swprintf_s(dllPath, L"%s\\%s", pluginsDir, wFilename);
    const std::string onDisk = PluginDllVersion::Read(dllPath);
    const std::string storedVersion = ReadPluginVersion(stateIniPath, sc.dllFilename.c_str());

    LogToFile::Debug("[AutoUpdate][Sidecar] '%s' stored=%s installed=%s remote=%s",
                     displayName,
                     storedVersion.empty() ? "<none>" : storedVersion.c_str(),
                     onDisk.empty() ? "<unreadable>" : onDisk.c_str(),
                     remoteVersion.c_str());

    // By number, so an older release never replaces a newer build. A version
    // that does not parse falls back to "differs".
    const AutoUpdateVersion::PluginUpdateDecision decision =
        AutoUpdateVersion::DecidePluginUpdate(storedVersion.c_str(), onDisk.c_str(), remoteVersion.c_str());
    if (decision.installed != storedVersion)
        WritePluginVersion(stateIniPath, sc.dllFilename.c_str(), decision.installed.c_str());

    if (decision.installed.empty())
    {
        LogToFile::Info("[AutoUpdate][Sidecar] '%s' — installed version not known yet (remote %s); "
                        "it is recorded when the plugin loads", displayName, remoteVersion.c_str());
        return;
    }

    if (!decision.download)
    {
        LogToFile::Info("[AutoUpdate][Sidecar] '%s' — up to date (installed %s, remote %s)",
                        displayName, decision.installed.c_str(), remoteVersion.c_str());
        return;
    }

    LogToFile::Info("[AutoUpdate][Sidecar] '%s' — update available: [%s] -> [%s]",
                    displayName, decision.installed.c_str(), remoteVersion.c_str());

#ifdef MODLOADER_CLIENT_BUILD
    {
        wchar_t msg[128];
        swprintf_s(msg, L"Updating %S (%d/%d)", displayName, index + 1, total);
        Splash::SetSubStatus(msg);
    }
#endif

    if (DownloadPlugin(downloadUrl.c_str(), pluginsDir, wFilename, displayName))
    {
        LogToFile::Info("[AutoUpdate][Sidecar] '%s' — updated to %s", displayName, remoteVersion.c_str());
        WritePluginVersion(stateIniPath, sc.dllFilename.c_str(), remoteVersion.c_str());
#ifdef MODLOADER_CLIENT_BUILD
        UI::UpdateNoticeWindow::AddUpdatedPlugin(displayName,
                                                 decision.installed.c_str(),
                                                 remoteVersion.c_str(),
                                                 AutoUpdateVersion::NotesForUpdate(
                                                     releaseNotes.c_str(), notes.c_str(),
                                                     decision.installed.c_str(), remoteVersion.c_str()).c_str());
#endif
    }
    else
    {
        LogToFile::Warn("[AutoUpdate][Sidecar] '%s' — download failed; existing file kept", displayName);
    }
}

// Scan the Plugins directory for sidecar JSON files and process each one.
static void RunPerPluginUpdates(const wchar_t* pluginsDir)
{
    auto sidecars = ScanForSidecars(pluginsDir);

    if (sidecars.empty())
    {
        LogToFile::Debug("[AutoUpdate][Sidecar] No plugin sidecars found in %ls", pluginsDir);
        return;

    }

    LogToFile::Info("[AutoUpdate][Sidecar] Found %zu plugin sidecar(s)", sidecars.size());

    wchar_t stateIniPath[MAX_PATH]{};
    GetUpdateStateIniPath(stateIniPath, MAX_PATH);

    int total = static_cast<int>(sidecars.size());
    for (int i = 0; i < total; ++i)
    {
#ifdef MODLOADER_CLIENT_BUILD
        wchar_t msg[128];
        swprintf_s(msg, L"Checking %S (%d/%d)", sidecars[i].dllFilename.c_str(), i + 1, total);
        Splash::SetSubStatus(msg);
        Splash::SetSubProgress(static_cast<float>(i) / static_cast<float>(total));
#endif
        ProcessPluginSidecar(sidecars[i], pluginsDir, stateIniPath, i, total);
    }

#ifdef MODLOADER_CLIENT_BUILD
    Splash::ClearSubBar();
#endif
}

// ===========================================================================
// Section F — Orchestrator
// ===========================================================================

#ifdef MODLOADER_CLIENT_BUILD
// List the loader in the "Plugins Updated" window, once, with the notes of the
// release it now is. The manifest the update check already fetched names the
// latest release; when that is the running version and its notes have not been
// shown yet, they go in the window. Two keys in update_state.ini carry this
// across launches: LoaderNotesPrevious, written here with the resolved previous
// version, and LoaderNotesShown, written when the window actually opens (which
// also removes LoaderNotesPrevious). A launch that quits or crashes before the
// main menu therefore leaves the notes, and the history, for the next one.
// Nothing is written when the fetch failed or the manifest names another
// version, so a later launch tries again.
static void ListLoaderNotesOnce(const char* running, const std::string& remoteTag, const std::string& manifest)
{
	char pending[256]{};
	ReadLoaderNotesPrevious(pending, sizeof(pending));
	char recorded[256]{};
	ReadLoaderNotesShown(recorded, sizeof(recorded));

	// The updater that installed this loader was the previous version's, and
	// wrote nothing for it to find but its log; see ResolvePreviousVersion.
	std::string currentLog, previousLog;
	if (!AutoUpdateVersion::ParseVersion(pending).valid && !AutoUpdateVersion::ParseVersion(recorded).valid)
		ReadUpdaterLogs(currentLog, previousLog);
	const std::string previous = AutoUpdateVersion::ResolvePreviousVersion(
		pending, recorded, currentLog.c_str(), previousLog.c_str(), running);

	const std::string releaseNotes = JsonExtractDecodedString(manifest, "release_notes");
	const std::string notes        = JsonExtractDecodedString(manifest, "notes");
	const AutoUpdateVersion::LoaderNotesDecision d = AutoUpdateVersion::DecideLoaderNotes(
		running, remoteTag.c_str(), previous.c_str(), releaseNotes.c_str(), notes.c_str());
	if (!d.show)
		return;

	// Keep the previous version until the window opens. If the ini cannot be
	// written the notes would come back on every launch, so they are not
	// listed at all.
	if (!WriteLoaderNotesKey(L"LoaderNotesPrevious", previous.c_str()))
		return;

	LogToFile::Info("[AutoUpdate] Listing the release notes of %s (previous version %s) in the Plugins Updated window",
	                remoteTag.c_str(), previous.empty() ? "unknown" : previous.c_str());
	UI::UpdateNoticeWindow::AddUpdatedPlugin(UI::UpdateNoticeWindow::kLoaderRowName, previous.c_str(), remoteTag.c_str(), d.text.c_str());
	UI::UpdateNoticeWindow::SetOnShown([tag = remoteTag]()
	{
		// Shown first: a crash between the two writes leaves a stale
		// LoaderNotesPrevious, which ResolvePreviousVersion ignores.
		if (WriteLoaderNotesKey(L"LoaderNotesShown", tag.c_str()))
			WriteLoaderNotesKey(L"LoaderNotesPrevious", nullptr);
	});
}
#endif

// Central manifest update pass — checks whether a new modloader release exists
// and notifies the user if so.  Plugin updates are handled independently via
// per-plugin sidecar manifests (Section G).
static void RunCentralManifestUpdate(const AutoUpdateConfig& cfg)
{
	if (cfg.manifestUrl[0] == '\0')
	{
		LogToFile::Info("[AutoUpdate] No manifest URL configured (dev / generic build) — skipping central update");
		return;
	}

	LogToFile::Info("[AutoUpdate] Manifest URL: %s%s",
	                cfg.manifestUrl,
	                cfg.urlFromIni ? " (from modloader.ini)" : " (compiled-in default)");

#ifdef MODLOADER_CLIENT_BUILD
	Splash::SetSubStatus(L"Fetching update manifest...");
	Splash::SetSubProgress(0.0f);
#endif

	std::string manifest = HttpGet(cfg.manifestUrl, "manifest");
	if (manifest.empty())
	{
		LogToFile::Warn("[AutoUpdate] Manifest fetch failed — skipping modloader version check");
#ifdef MODLOADER_CLIENT_BUILD
		Splash::SetSubStatus(L"Update check failed (no connection?)");
#endif
		return;
	}

	LogToFile::Debug("[AutoUpdate] Manifest received (%zu bytes)", manifest.size());

	std::string remoteBuildTag = JsonExtractString(manifest, "build_tag");
	if (remoteBuildTag.empty())
	{
		LogToFile::Warn("[AutoUpdate] Manifest is missing 'build_tag' field — skipping");
		return;
	}

	LogToFile::Debug("[AutoUpdate] Remote build_tag: %s", remoteBuildTag.c_str());

	// The build tag stamped into this DLL by CI (via /p:ModLoaderBuildTag=...).
	// Empty on dev/generic builds.
#ifdef MODLOADER_BUILD_TAG
	const char* compiledTag = MODLOADER_BUILD_TAG;
#else
	const char* compiledTag = "";
#endif

#ifdef MODLOADER_CLIENT_BUILD
	ListLoaderNotesOnce(compiledTag, remoteBuildTag, manifest);
#endif

	// Read stored tag (written by a previous run) and choose the effective local version.
	char storedTag[256]{};
	ReadStoredBuildTag(storedTag, sizeof(storedTag));
	LogToFile::Debug("[AutoUpdate] Stored build_tag:   %s", storedTag[0] ? storedTag : "<none>");
	LogToFile::Debug("[AutoUpdate] Compiled build_tag: %s", compiledTag[0] ? compiledTag : "<none>");

	// The tag compiled into this running Core DLL is ground truth -- it
	// cannot lie about what's actually installed.  update_state.ini's
	// BuildTag can go stale (e.g. a manual reinstall of the latest release
	// doesn't necessarily touch it), so only fall back to it on dev/generic
	// builds where no build tag is compiled in at all.
	const char* effectiveLocalTag = compiledTag[0] ? compiledTag : storedTag;

	// By number, as the updater exe decides: a build newer than the release
	// (a pre-release or a local build) is up to date, not offered an older one.
	const AutoUpdateVersion::ParsedVersion localVer  = AutoUpdateVersion::ParseVersion(effectiveLocalTag);
	const AutoUpdateVersion::ParsedVersion remoteVer = AutoUpdateVersion::ParseVersion(remoteBuildTag.c_str());
	if (localVer.valid && remoteVer.valid ? AutoUpdateVersion::CompareVersions(remoteVer, localVer) <= 0
	                                      : strcmp(effectiveLocalTag, remoteBuildTag.c_str()) == 0)
	{
		LogToFile::Info("[AutoUpdate] Modloader is up to date (installed %s, remote %s)",
		                effectiveLocalTag, remoteBuildTag.c_str());

		// Keep update_state.ini in sync with the real compiled tag so it
		// never drives a stale comparison later (e.g. the standalone
		// updater exe's file-version-resource-unavailable fallback path).
		if (compiledTag[0] != '\0' && strcmp(storedTag, compiledTag) != 0)
		{
			LogToFile::Debug("[AutoUpdate] update_state.ini out of sync with running build -- refreshing");
			WriteStoredBuildTag(compiledTag);
		}
#ifdef MODLOADER_CLIENT_BUILD
		Splash::SetSubStatus(L"Modloader is up to date");
		Splash::SetSubProgress(1.0f);
#endif
		return;
	}

	LogToFile::Info("[AutoUpdate] Modloader update available: [%s] -> [%s]",
	                effectiveLocalTag[0] ? effectiveLocalTag : "<none>", remoteBuildTag.c_str());

#ifdef MODLOADER_CLIENT_BUILD
	UI::GlobalSettings::SetUpdateAvailable(true);
	Splash::SetSubStatus(L"Modloader update available!");
	Splash::SetSubProgress(1.0f);
#endif

	// Note: update_state.ini's BuildTag is intentionally NOT written here.
	// Core never installs the update itself (only the standalone updater
	// exe does, after the user accepts), so writing the remote tag here
	// would falsely record an update that may never actually be installed
	// -- and that same key is the updater exe's own fallback source of
	// truth when it can't read the Core DLL's file version resource.
}

bool AutoUpdateEnabled()
{
	return ReadAutoUpdateConfig().enabled;
}

// The feed URL is a compiled define of its own and never read from modloader.ini.
#ifndef VMAP_FEED_URL
#  define VMAP_FEED_URL ""
#endif

namespace
{
	constexpr bool EndsWith(const char* s, const char* tail)
	{
		size_t n = 0, t = 0;
		while (s[n]) ++n;
		while (tail[t]) ++t;
		for (size_t i = 0; i < t; ++i)
			if (n < t || s[n - t + i] != tail[i])
				return false;
		return true;
	}
}

// The asset name carries the feed format, so a later format gets a new name
// and never strands a loader that reads this one.
static_assert(VersionMap::kFeedFormat == 1 && (VMAP_FEED_URL[0] == 0 || EndsWith(VMAP_FEED_URL, "/version-map-client.v1.txt")),
	"VMAP_FEED_URL must name version-map-client.v<kFeedFormat>.txt");

FeedFetch::Config FeedFetch::LoaderConfig()
{
	Config cfg;
#ifdef MODLOADER_CLIENT_BUILD
	cfg.client = true;
#endif
	cfg.url = VMAP_FEED_URL;
	cfg.keys = VersionFeed::CompiledKeys();
	cfg.cachePath = GetModLoaderDirPath(L"version-map-client.v1.cache");
	cfg.autoUpdate = AutoUpdateEnabled();
	return cfg;
}

void ModLoaderLogger::RunAutoUpdate()
{
#ifdef MODLOADER_BUILD_TAG
	LogToFile::Info("[AutoUpdate] Starting auto-update check (current version: %s)", MODLOADER_BUILD_TAG);
#else
	LogToFile::Info("[AutoUpdate] Starting auto-update check (current version: unknown)");
#endif

	AutoUpdateConfig cfg = ReadAutoUpdateConfig();

	if (!cfg.enabled)
	{
		LogToFile::Info("[AutoUpdate] Disabled via modloader.ini — skipping");
		return;
	}

	// Determine and ensure the Plugins directory exists.  Both the central
	// manifest pass and the per-plugin sidecar pass need it.
	wchar_t pluginsDir[MAX_PATH]{};
	GetModuleFileNameW(nullptr, pluginsDir, MAX_PATH);
	wchar_t* slash = wcsrchr(pluginsDir, L'\\');
	if (slash)
		wcscpy_s(slash + 1,
		         MAX_PATH - static_cast<DWORD>(slash + 1 - pluginsDir),
		         L"ModLoader\\Plugins");
	LogToFile::Debug("[AutoUpdate] Plugins directory: %ls", pluginsDir);
	{
		wchar_t modloaderPath[MAX_PATH]{};
		wcscpy_s(modloaderPath, pluginsDir);
		if (wchar_t* s = wcsrchr(modloaderPath, L'\\')) *s = L'\0';
		CreateDirectoryW(modloaderPath, nullptr);
	}
	CreateDirectoryW(pluginsDir, nullptr);

	// Central manifest pass — checks for a new modloader release and notifies
	// the user.  Skipped on dev builds (no URL).
#ifdef MODLOADER_CLIENT_BUILD
	Splash::SetSubStatus(L"Checking for modloader update...");
	Splash::SetSubProgress(0.0f);
#endif
	RunCentralManifestUpdate(cfg);

	// Per-plugin sidecar pass — updates any plugin that ships a .json sidecar
	// pointing at its own manifest.  Runs regardless of whether a central
	// manifest URL is configured, so third-party plugins always get checked.
#ifdef MODLOADER_CLIENT_BUILD
	Splash::SetSubStatus(L"Checking for plugin updates...");
	Splash::SetSubProgress(0.0f);
#endif
	RunPerPluginUpdates(pluginsDir);

	// The signed version-map feed, for the next launch. The server never asks.
#ifdef MODLOADER_CLIENT_BUILD
	FeedFetch::Stage3(FeedFetch::LoaderConfig());
#endif
}
