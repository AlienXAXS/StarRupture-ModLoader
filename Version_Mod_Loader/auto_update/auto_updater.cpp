// auto_updater.cpp
// Plugin auto-update system — see auto_updater.h for overview.

#include "auto_updater.h"
#include "logging/log.h"
#include "plugins/plugin_interface.h"

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

	Log::Debug("[AutoUpdate] Reading config from: %ls", iniPath);

	cfg.enabled = (GetPrivateProfileIntW(L"AutoUpdate", L"Enabled", 1, iniPath) != 0);
	Log::Debug("[AutoUpdate] Enabled=%s (from ini)", cfg.enabled ? "true" : "false");

	wchar_t wUrl[512]{};
	GetPrivateProfileStringW(L"AutoUpdate", L"ManifestUrl", L"", wUrl, 512, iniPath);

	if (wUrl[0] == L'\0')
	{
		// Fall back to the compiled-in default (set by CI)
		strncpy_s(cfg.manifestUrl, sizeof(cfg.manifestUrl),
			AUTOUPDATE_DEFAULT_MANIFEST_URL, _TRUNCATE);
		cfg.urlFromIni = false;
		Log::Debug("[AutoUpdate] ManifestUrl: using compiled-in default");
	}
	else
	{
		WideCharToMultiByte(CP_UTF8, 0, wUrl, -1,
			cfg.manifestUrl, sizeof(cfg.manifestUrl),
			nullptr, nullptr);
		cfg.urlFromIni = true;
		Log::Debug("[AutoUpdate] ManifestUrl: overridden via modloader.ini");
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
			static_cast<rsize_t>(maxLen - static_cast<DWORD>(slash + 1 - outPath)),
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
		Log::Debug("[AutoUpdate] Stored build tag written: %s", tag);
	else
	{
		DWORD err = GetLastError();
		Log::Warn("[AutoUpdate] Failed to write build tag to update_state.ini (%lu: %s)", err, FormatWinHttpError(err).c_str());
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
			hWinHttp, err, 0, buf, static_cast<DWORD>(sizeof(buf) - 1), nullptr);
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
		nullptr, err, 0, buf, static_cast<DWORD>(sizeof(buf) - 1), nullptr);
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
	Log::Debug("[AutoUpdate] HttpGet [%s]: %s", label, url);

	// Convert URL to wide string
	wchar_t wUrl[1024]{};
	if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 1024) == 0)
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: URL encoding failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
		return {};
	}

	// Crack the URL into host + path components
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256]{}, path[768]{};
	uc.lpszHostName   = host; uc.dwHostNameLength   = 256;
	uc.lpszUrlPath    = path; uc.dwUrlPathLength    = 768;
	if (!WinHttpCrackUrl(wUrl, 0, 0, &uc))
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpCrackUrl failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
		return {};
	}

	Log::Debug("[AutoUpdate] HttpGet [%s]: host=%ls path=%ls port=%d", label, host, path, (int)uc.nPort);

	// WINHTTP_ACCESS_TYPE_NO_PROXY: connect directly without reading IE/user
	// proxy registry settings.  Dedicated game servers run as SYSTEM which has
	// no IE proxy hive, causing WinHttpSendRequest to fail with error 5023
	// (ERROR_WINHTTP_CANNOT_CONNECT) when the default proxy mode is used.
	HINTERNET hSession = WinHttpOpen(
		L"StarRupture-ModLoader-AutoUpdate/1.0",
		WINHTTP_ACCESS_TYPE_NO_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession)
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpOpen failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
		return {};
	}

	// Cap individual operation timeouts so we never block startup indefinitely.
	// resolve=5s, connect=10s, send=30s, receive=30s
	WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 30000);

	INTERNET_PORT port = (uc.nPort != 0) ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
	HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
	if (!hConnect)
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpConnect failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hSession);
		return {};
	}

	DWORD reqFlags = WINHTTP_FLAG_SECURE;
	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect, L"GET", path,
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
	if (!hRequest)
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpOpenRequest failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	// Follow GitHub CDN redirects automatically
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
		&redirectPolicy, sizeof(redirectPolicy));

	Log::Debug("[AutoUpdate] HttpGet [%s]: sending request...", label);

	if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpSendRequest failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	if (!WinHttpReceiveResponse(hRequest, nullptr))
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpReceiveResponse failed (%lu: %s)", label, err, FormatWinHttpError(err).c_str());
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
		WINHTTP_HEADER_NAME_BY_INDEX,
		&statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

	Log::Debug("[AutoUpdate] HttpGet [%s]: HTTP %lu", label, statusCode);

	if (statusCode != 200)
	{
		// Promote to Warn so HTTP errors are always visible without debug logging.
		// Common causes:
		//   404 — release does not exist or is a private/draft release
		//   403 — asset requires authentication (private repo)
		//   5xx — GitHub CDN or release server error
		Log::Warn("[AutoUpdate] HttpGet [%s]: server returned HTTP %lu (URL: %s)", label, statusCode, url);
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	// Read response body in chunks
	std::string body;
	body.reserve(65536);

	DWORD available = 0;
	while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
	{
		std::string chunk(available, '\0');
		DWORD bytesRead = 0;
		if (!WinHttpReadData(hRequest, chunk.data(), available, &bytesRead))
		{
			DWORD err = GetLastError();
			Log::Debug("[AutoUpdate] HttpGet [%s]: WinHttpReadData failed mid-stream (%lu: %s) — %zu bytes received so far",
				label, err, FormatWinHttpError(err).c_str(), body.size());
			break;
		}
		chunk.resize(bytesRead);
		body += chunk;
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	Log::Debug("[AutoUpdate] HttpGet [%s]: received %zu bytes", label, body.size());
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
		if (json[i] == ' '  || json[i] == '\t' ||
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
					if      (c == '"') { inString = true; }
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
	const char*    displayName)
{
	wchar_t tmpPath[MAX_PATH]{};
	wchar_t finalPath[MAX_PATH]{};
	swprintf_s(tmpPath,   L"%s\\%s.tmp", pluginsDir, filename);
	swprintf_s(finalPath, L"%s\\%s",     pluginsDir, filename);

	std::string body = HttpGet(url, displayName);
	if (body.empty())
	{
		Log::Debug("[AutoUpdate] Download [%s]: HttpGet returned empty body", displayName);
		return false;
	}

	Log::Debug("[AutoUpdate] Download [%s]: writing %zu bytes to temp file", displayName, body.size());

	HANDLE hFile = CreateFileW(tmpPath, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] Download [%s]: CreateFileW failed for temp path (%lu: %s): %ls",
			displayName, err, FormatWinHttpError(err).c_str(), tmpPath);
		return false;
	}

	DWORD written = 0;
	BOOL ok = WriteFile(hFile, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
	DWORD writeErr = GetLastError();
	CloseHandle(hFile);

	if (!ok || written != static_cast<DWORD>(body.size()))
	{
		Log::Debug("[AutoUpdate] Download [%s]: WriteFile incomplete — wrote %lu of %zu bytes (error %lu)",
			displayName, written, body.size(), writeErr);
		DeleteFileW(tmpPath);
		return false;
	}

	Log::Debug("[AutoUpdate] Download [%s]: %lu bytes written, renaming to final path", displayName, written);

	// Atomic rename: replace existing in one operation so the original is
	// never removed unless the new file is successfully placed.
	if (!MoveFileExW(tmpPath, finalPath, MOVEFILE_REPLACE_EXISTING))
	{
		DWORD err = GetLastError();
		Log::Debug("[AutoUpdate] Download [%s]: MoveFileExW failed (%lu: %s) — temp file removed",
			displayName, err, FormatWinHttpError(err).c_str());
		DeleteFileW(tmpPath);
		return false;
	}

	Log::Debug("[AutoUpdate] Download [%s]: placed at %ls", displayName, finalPath);
	return true;
}

// ===========================================================================
// Section F — Orchestrator
// ===========================================================================

void ModLoader::RunAutoUpdate()
{
	Log::Info("[AutoUpdate] Starting auto-update check");

	AutoUpdateConfig cfg = ReadAutoUpdateConfig();

	if (!cfg.enabled)
	{
		Log::Info("[AutoUpdate] Disabled via modloader.ini — skipping");
		return;
	}

	if (cfg.manifestUrl[0] == '\0')
	{
		Log::Info("[AutoUpdate] No manifest URL configured (dev / generic build) — skipping");
		return;
	}

	Log::Info("[AutoUpdate] Manifest URL: %s%s",
		cfg.manifestUrl,
		cfg.urlFromIni ? " (from modloader.ini)" : " (compiled-in default)");

	// Derive the base download URL by stripping the manifest filename.
	// e.g. ".../releases/download/TAG/manifest-server.json"
	//   -> ".../releases/download/TAG/"
	// Individual plugin DLLs are then fetched as baseDownloadUrl + filename.
	std::string baseDownloadUrl = cfg.manifestUrl;
	{
		size_t lastSlash = baseDownloadUrl.rfind('/');
		if (lastSlash != std::string::npos)
			baseDownloadUrl.resize(lastSlash + 1); // keep trailing '/'
	}
	Log::Debug("[AutoUpdate] Base download URL: %s", baseDownloadUrl.c_str());

	std::string manifest = HttpGet(cfg.manifestUrl, "manifest");
	if (manifest.empty())
	{
		Log::Warn("[AutoUpdate] Manifest fetch failed — see above for details (network error or HTTP error)");
		Log::Info("[AutoUpdate] Skipping update; existing plugins will be loaded as-is");
		return;
	}

	Log::Debug("[AutoUpdate] Manifest received (%zu bytes)", manifest.size());

	// Gate on interface version
	int manifestIfaceVer = JsonExtractInt(manifest, "interface_version", -1);
	Log::Debug("[AutoUpdate] Manifest interface_version=%d, loader PLUGIN_INTERFACE_VERSION=%d",
		manifestIfaceVer, PLUGIN_INTERFACE_VERSION);

	if (manifestIfaceVer != PLUGIN_INTERFACE_VERSION)
	{
		Log::Warn("[AutoUpdate] Interface version mismatch — manifest=%d, loader=%d",
			manifestIfaceVer, PLUGIN_INTERFACE_VERSION);
		Log::Info("[AutoUpdate] Skipping update; plugins built for a different interface version "
			"cannot be safely loaded by this loader");
		return;
	}

	std::string remoteBuildTag = JsonExtractString(manifest, "build_tag");
	if (remoteBuildTag.empty())
	{
		Log::Warn("[AutoUpdate] Manifest is missing 'build_tag' field — skipping update");
		return;
	}

	Log::Debug("[AutoUpdate] Remote build_tag: %s", remoteBuildTag.c_str());

	// Determine plugins directory
	wchar_t pluginsDir[MAX_PATH]{};
	GetModuleFileNameW(nullptr, pluginsDir, MAX_PATH);
	wchar_t* slash = wcsrchr(pluginsDir, L'\\');
	if (slash)
		wcscpy_s(slash + 1,
			static_cast<rsize_t>(MAX_PATH - static_cast<DWORD>(slash + 1 - pluginsDir)),
			L"Plugins");

	Log::Debug("[AutoUpdate] Plugins directory: %ls", pluginsDir);

	// Ensure Plugins directory exists (in case we're running before first load)
	CreateDirectoryW(pluginsDir, nullptr);

	// The build tag stamped into this DLL by CI (via /p:ModLoaderBuildTag=...).
	// Empty on dev/generic builds.  Used as a fallback when update_state.ini
	// does not exist yet (fresh install from a ZIP archive).
#ifdef MODLOADER_BUILD_TAG
	const char* compiledTag = MODLOADER_BUILD_TAG;
#else
	const char* compiledTag = "";
#endif

	// Read stored tag (written by a previous auto-update run)
	char storedTag[256]{};
	ReadStoredBuildTag(storedTag, sizeof(storedTag));
	Log::Debug("[AutoUpdate] Stored build_tag:   %s", storedTag[0] ? storedTag : "<none>");
	Log::Debug("[AutoUpdate] Compiled build_tag: %s", compiledTag[0] ? compiledTag : "<none>");

	// Determine the effective local version:
	//   • If update_state.ini exists, use it — it tracks what the auto-updater
	//     last downloaded and is authoritative after the first run.
	//   • Otherwise fall back to the tag compiled into this DLL.  On a fresh
	//     install from a ZIP the DLL and all plugin DLLs share the same build
	//     tag, so if it matches the manifest there is nothing to download.
	const char* effectiveLocalTag = storedTag[0] ? storedTag : compiledTag;

	// Parse plugin list
	auto pluginEntries = JsonExtractObjectArray(manifest, "plugins");
	Log::Info("[AutoUpdate] Manifest lists %zu plugin(s)", pluginEntries.size());

	if (pluginEntries.empty())
	{
		Log::Warn("[AutoUpdate] Manifest contains no plugin entries — nothing to update");
		return;
	}

	// Log the full plugin list at debug level
	for (size_t idx = 0; idx < pluginEntries.size(); ++idx)
	{
		std::string n = JsonExtractString(pluginEntries[idx], "name");
		std::string f = JsonExtractString(pluginEntries[idx], "filename");
		Log::Debug("[AutoUpdate]   [%zu] name='%s'  filename='%s'  url='%s%s'",
			idx, n.c_str(), f.c_str(), baseDownloadUrl.c_str(), f.c_str());
	}

	bool tagsMatch = (effectiveLocalTag[0] != '\0' &&
	                  strcmp(effectiveLocalTag, remoteBuildTag.c_str()) == 0);

	if (tagsMatch)
	{
		// Already up to date — plugins on disk match the manifest version.
		Log::Info("[AutoUpdate] Already up to date (%s)", effectiveLocalTag);

		// If we matched via the compiled tag rather than a stored tag, this is
		// the first boot after a fresh install from a ZIP.  Write update_state.ini
		// now so future boots skip the manifest comparison entirely.
		if (storedTag[0] == '\0' && compiledTag[0] != '\0')
		{
			Log::Debug("[AutoUpdate] First run after fresh install — writing update_state.ini");
			WriteStoredBuildTag(compiledTag);
		}

		// Log per-plugin status at debug level for diagnostics
		for (const auto& entry : pluginEntries)
		{
			std::string filename = JsonExtractString(entry, "filename");
			if (filename.empty()) continue;

			wchar_t wFilename[256]{};
			MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wFilename, 256);

			wchar_t dllPath[MAX_PATH]{};
			swprintf_s(dllPath, L"%s\\%s", pluginsDir, wFilename);

			if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
				Log::Debug("[AutoUpdate]   '%s' — not installed, skipping", filename.c_str());
			else
				Log::Debug("[AutoUpdate]   '%s' — installed and up to date", filename.c_str());
		}

		return;
	}

	Log::Info("[AutoUpdate] Update available: [%s] -> [%s]",
		effectiveLocalTag[0] ? effectiveLocalTag : "<none>", remoteBuildTag.c_str());

	// Download each plugin entry — but ONLY if the user already has it installed.
	// Plugins absent from disk were deliberately not installed and must not be
	// force-downloaded just because they appear in the manifest.
	int downloaded = 0;
	int skipped    = 0;
	int failed     = 0;

	for (const auto& entry : pluginEntries)
	{
		std::string name        = JsonExtractString(entry, "name");
		std::string filename    = JsonExtractString(entry, "filename");
		const char* displayName = name.empty() ? filename.c_str() : name.c_str();

		if (filename.empty())
		{
			Log::Warn("[AutoUpdate] Skipping malformed plugin entry — missing 'filename'");
			++failed;
			continue;
		}

		// Build the download URL from the base (derived from the manifest URL)
		// and the plugin's filename, e.g. ".../TAG/ServerUtility-Server.dll"
		std::string downloadUrl = baseDownloadUrl + filename;

		wchar_t wFilename[256]{};
		MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wFilename, 256);

		// First gate: only update plugins the user has chosen to install
		wchar_t dllPath[MAX_PATH]{};
		swprintf_s(dllPath, L"%s\\%s", pluginsDir, wFilename);
		if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
		{
			Log::Debug("[AutoUpdate] '%s' — not installed on disk, skipping", displayName);
			++skipped;
			continue;
		}

		Log::Info("[AutoUpdate] Updating %s ...", displayName);

		if (DownloadPlugin(downloadUrl.c_str(), pluginsDir, wFilename, displayName))
		{
			Log::Info("[AutoUpdate] %s — updated successfully", displayName);
			++downloaded;
		}
		else
		{
			Log::Warn("[AutoUpdate] %s — update FAILED; existing file will be kept and loaded", displayName);
			++failed;
		}
	}

	Log::Info("[AutoUpdate] Pass complete: %d downloaded, %d skipped, %d failed",
		downloaded, skipped, failed);

	// Only persist the new build tag when everything succeeded
	if (failed == 0)
	{
		WriteStoredBuildTag(remoteBuildTag.c_str());
		Log::Info("[AutoUpdate] Build tag updated to [%s]", remoteBuildTag.c_str());
	}
	else
	{
		Log::Warn("[AutoUpdate] %d plugin(s) failed to download — build tag NOT updated; "
			"update will be retried on next boot", failed);
		Log::Info("[AutoUpdate] Existing plugins on disk will be loaded as-is");
	}
}
