// auto_updater.cpp
// Plugin auto-update system — see auto_updater.h for overview.

#include "auto_updater.h"
#include "logging/log.h"
#include "plugins/plugin_interface.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/global_settings.h"
#include "UI/splash_window.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// Forward declaration — defined in Section C
static std::string FormatWinHttpError(DWORD err);

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
		wcscpy_s(slash + 1, static_cast<rsize_t>(MAX_PATH - (slash + 1 - iniPath)), L"modloader.ini");

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
// Section B — Update state (Plugins\update_state.ini)
// ===========================================================================

static void GetUpdateStateIniPath(wchar_t* outPath, DWORD maxLen)
{
	GetModuleFileNameW(nullptr, outPath, maxLen);
	wchar_t* slash = wcsrchr(outPath, L'\\');
	if (slash)
		wcscpy_s(slash + 1,
		         maxLen - static_cast<DWORD>(slash + 1 - outPath),
		         L"update_state.ini");
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

// ===========================================================================
// Section C — WinHTTP GET
// ===========================================================================

// Converts a WinHTTP (or Win32) error code to a human-readable string.
// Checks the WinHTTP message table first, then falls back to the system table.
static std::string FormatWinHttpError(DWORD err)
{
	char buf[512]{};
	HMODULE hWinHttp = GetModuleHandleW(L"winhttp.dll");

	if (hWinHttp)
	{
		DWORD n = FormatMessageA(
			FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
			hWinHttp, err, 0, buf, sizeof(buf) - 1, nullptr);
		if (n > 0)
		{
			// Strip trailing CR/LF/spaces inserted by FormatMessage
			while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
				buf[--n] = '\0';
			return std::string(buf);
		}
	}

	// Fall back to the system error message table
	DWORD n = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
	if (n > 0)
	{
		while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
			buf[--n] = '\0';
		return std::string(buf);
	}

	return "unknown error " + std::to_string(err);
}

// Returns the full HTTP response body on success, or an empty string on any
// error.  Handles HTTPS and follows redirects (required for GitHub CDN).
// `label` is used only in log messages to identify what's being fetched.
static std::string HttpGet(const char* url, const char* label = "resource")
{
	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: %s", label, url);

	// Convert URL to wide string
	wchar_t wUrl[1024]{};
	if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 1024) == 0)
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: URL encoding failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		return {};
	}

	// Crack the URL into host + path components
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256]{}, path[768]{};
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 768;
	if (!WinHttpCrackUrl(wUrl, 0, 0, &uc))
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: WinHttpCrackUrl failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		return {};
	}

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: host=%ls path=%ls port=%d", label, host, path,
	                 static_cast<int>(uc.nPort));

	// WINHTTP_ACCESS_TYPE_NO_PROXY: connect directly without reading IE/user
	// proxy registry settings.  Dedicated game servers run as SYSTEM which has
	// no IE proxy hive, causing WinHttpSendRequest to fail with error 5023
	// (ERROR_WINHTTP_CANNOT_CONNECT) when the default proxy mode is used.
	HINTERNET hSession = WinHttpOpen(
		L"StarRupture-ModLoader-AutoUpdate/1.0",
		WINHTTP_ACCESS_TYPE_NO_PROXY,
		nullptr, nullptr, 0);
	if (!hSession)
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: WinHttpOpen failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		return {};
	}

	// Cap individual operation timeouts so we never block startup indefinitely.
	// resolve=3s, connect=5s (~8s max for unreachable host), send=15s, receive=30s
	WinHttpSetTimeouts(hSession, 3000, 5000, 15000, 30000);

	INTERNET_PORT port = (uc.nPort != 0) ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
	HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
	if (!hConnect)
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: WinHttpConnect failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hSession);
		return {};
	}

	DWORD reqFlags = WINHTTP_FLAG_SECURE;
	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect, L"GET", path,
		nullptr, nullptr, nullptr, reqFlags);
	if (!hRequest)
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: WinHttpOpenRequest failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	// Follow GitHub CDN redirects automatically
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
	                 &redirectPolicy, sizeof(redirectPolicy));

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: sending request...", label);

	if (!WinHttpSendRequest(hRequest, nullptr, 0,
	                        nullptr, 0, 0, 0))
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: WinHttpSendRequest failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	if (!WinHttpReceiveResponse(hRequest, nullptr))
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: WinHttpReceiveResponse failed (%lu: %s)", label, err,
		                 FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	// Check HTTP status code
	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	WinHttpQueryHeaders(hRequest,
	                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                    nullptr,
	                    &statusCode, &statusSize, nullptr);

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: HTTP %lu", label, statusCode);

	if (statusCode != 200)
	{
		// Promote to Warn so HTTP errors are always visible without debug logging.
		// Common causes:
		//   404 — release does not exist or is a private/draft release
		//   403 — asset requires authentication (private repo)
		//   5xx — GitHub CDN or release server error
		LogToFile::Warn("[AutoUpdate] HttpGet [%s]: server returned HTTP %lu (URL: %s)", label, statusCode, url);
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	// Read response body in chunks
	std::string body;
	body.reserve(65536);

	bool readOk = true;
	for (;;)
	{
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(hRequest, &available))
		{
			DWORD err = GetLastError();
			LogToFile::Warn(
				"[AutoUpdate] HttpGet [%s]: WinHttpQueryDataAvailable failed (%lu: %s) -- %zu bytes received so far",
				label, err, FormatWinHttpError(err).c_str(), body.size());
			readOk = false;
			break;
		}
		if (available == 0)
			break; // normal end of response

		std::string chunk(available, '\0');
		DWORD bytesRead = 0;
		if (!WinHttpReadData(hRequest, chunk.data(), available, &bytesRead))
		{
			DWORD err = GetLastError();
			LogToFile::Warn(
				"[AutoUpdate] HttpGet [%s]: WinHttpReadData failed mid-stream (%lu: %s) -- %zu bytes received so far",
				label, err, FormatWinHttpError(err).c_str(), body.size());
			readOk = false;
			break;
		}
		chunk.resize(bytesRead);
		body += chunk;
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	if (!readOk)
	{
		LogToFile::Warn("[AutoUpdate] HttpGet [%s]: incomplete download -- discarding partial data to avoid corrupt file on disk", label);
		return {};
	}

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: received %zu bytes", label, body.size());
	return body;
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
//     "download_url":          "https://example.com/releases/MyPlugin-1.2.0.dll"
//   }
//
// The last-downloaded version per plugin is stored in update_state.ini under
// [PluginVersions], keyed by the DLL filename (e.g. "MyPlugin.dll=1.2.0").
// If the remote version differs from the stored version the DLL is replaced
// using the same atomic .tmp -> rename pattern as the central updater.
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

// Fetch and process a single per-plugin manifest.
// Downloads the plugin DLL if the remote version differs from stored.
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

    std::string storedVersion = ReadPluginVersion(stateIniPath, sc.dllFilename.c_str());

    LogToFile::Debug("[AutoUpdate][Sidecar] '%s' stored=%s remote=%s",
                     displayName,
                     storedVersion.empty() ? "<none>" : storedVersion.c_str(),
                     remoteVersion.c_str());

    if (!storedVersion.empty() && storedVersion == remoteVersion)
    {
        LogToFile::Info("[AutoUpdate][Sidecar] '%s' — up to date (%s)", displayName, remoteVersion.c_str());
        return;
    }

    LogToFile::Info("[AutoUpdate][Sidecar] '%s' — update available: [%s] -> [%s]",
                    displayName,
                    storedVersion.empty() ? "<none>" : storedVersion.c_str(),
                    remoteVersion.c_str());

#ifdef MODLOADER_CLIENT_BUILD
    {
        wchar_t msg[128];
        swprintf_s(msg, L"Updating %S (%d/%d)", displayName, index + 1, total);
        Splash::SetSubStatus(msg);
    }
#endif

    wchar_t wFilename[256]{};
    MultiByteToWideChar(CP_UTF8, 0, sc.dllFilename.c_str(), -1, wFilename, 256);

    if (DownloadPlugin(downloadUrl.c_str(), pluginsDir, wFilename, displayName))
    {
        LogToFile::Info("[AutoUpdate][Sidecar] '%s' — updated to %s", displayName, remoteVersion.c_str());
        WritePluginVersion(stateIniPath, sc.dllFilename.c_str(), remoteVersion.c_str());
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

	std::string manifest = HttpGet(cfg.manifestUrl, "manifest");
	if (manifest.empty())
	{
		LogToFile::Warn("[AutoUpdate] Manifest fetch failed — skipping modloader version check");
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

	// Read stored tag (written by a previous run) and choose the effective local version.
	char storedTag[256]{};
	ReadStoredBuildTag(storedTag, sizeof(storedTag));
	LogToFile::Debug("[AutoUpdate] Stored build_tag:   %s", storedTag[0] ? storedTag : "<none>");
	LogToFile::Debug("[AutoUpdate] Compiled build_tag: %s", compiledTag[0] ? compiledTag : "<none>");

	const char* effectiveLocalTag = storedTag[0] ? storedTag : compiledTag;

	if (effectiveLocalTag[0] != '\0' &&
		strcmp(effectiveLocalTag, remoteBuildTag.c_str()) == 0)
	{
		LogToFile::Info("[AutoUpdate] Modloader is up to date (%s)", effectiveLocalTag);

		// First boot after a fresh install from a ZIP — persist the tag so
		// future boots skip the fetch entirely once no update is available.
		if (storedTag[0] == '\0' && compiledTag[0] != '\0')
		{
			LogToFile::Debug("[AutoUpdate] First run after fresh install — writing update_state.ini");
			WriteStoredBuildTag(compiledTag);
		}
		return;
	}

	LogToFile::Info("[AutoUpdate] Modloader update available: [%s] -> [%s]",
	                effectiveLocalTag[0] ? effectiveLocalTag : "<none>", remoteBuildTag.c_str());

#ifdef MODLOADER_CLIENT_BUILD
	UI::GlobalSettings::SetUpdateAvailable(true);
#endif

	// Write the new tag so the notification is not shown again until the
	// user actually updates (i.e. replaces dwmapi.dll with the new build).
	WriteStoredBuildTag(remoteBuildTag.c_str());
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
		         L"Plugins");
	LogToFile::Debug("[AutoUpdate] Plugins directory: %ls", pluginsDir);
	CreateDirectoryW(pluginsDir, nullptr);

	// Central manifest pass — checks for a new modloader release and notifies
	// the user.  Skipped on dev builds (no URL).
	RunCentralManifestUpdate(cfg);

	// Per-plugin sidecar pass — updates any plugin that ships a .json sidecar
	// pointing at its own manifest.  Runs regardless of whether a central
	// manifest URL is configured, so third-party plugins always get checked.
	RunPerPluginUpdates(pluginsDir);
}
