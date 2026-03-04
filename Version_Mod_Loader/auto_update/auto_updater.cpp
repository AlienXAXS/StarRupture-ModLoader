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

// ===========================================================================
// Section A — Configuration
// ===========================================================================

struct AutoUpdateConfig
{
	bool enabled = true;
	char manifestUrl[512] = {};
};

static AutoUpdateConfig ReadAutoUpdateConfig()
{
	AutoUpdateConfig cfg;

	wchar_t iniPath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
	wchar_t* slash = wcsrchr(iniPath, L'\\');
	if (slash)
		wcscpy_s(slash + 1, static_cast<rsize_t>(MAX_PATH - (slash + 1 - iniPath)), L"modloader.ini");

	cfg.enabled = (GetPrivateProfileIntW(L"AutoUpdate", L"Enabled", 1, iniPath) != 0);

	wchar_t wUrl[512]{};
	GetPrivateProfileStringW(L"AutoUpdate", L"ManifestUrl", L"", wUrl, 512, iniPath);

	if (wUrl[0] == L'\0')
	{
		// Fall back to the compiled-in default (set by CI)
		strncpy_s(cfg.manifestUrl, sizeof(cfg.manifestUrl),
			AUTOUPDATE_DEFAULT_MANIFEST_URL, _TRUNCATE);
	}
	else
	{
		WideCharToMultiByte(CP_UTF8, 0, wUrl, -1,
			cfg.manifestUrl, sizeof(cfg.manifestUrl),
			nullptr, nullptr);
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
			L"Plugins\\update_state.ini");
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

	WritePrivateProfileStringW(L"AutoUpdate", L"BuildTag", wTag, iniPath);
}

// ===========================================================================
// Section C — WinHTTP GET
// ===========================================================================

// Returns the full HTTP response body on success, or an empty string on any
// error.  Handles HTTPS and follows redirects (required for GitHub CDN).
static std::string HttpGet(const char* url)
{
	// Convert URL to wide string
	wchar_t wUrl[1024]{};
	if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl, 1024) == 0)
		return {};

	// Crack the URL into host + path components
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256]{}, path[768]{};
	uc.lpszHostName = host; uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path; uc.dwUrlPathLength = 768;
	if (!WinHttpCrackUrl(wUrl, 0, 0, &uc))
		return {};

	HINTERNET hSession = WinHttpOpen(
		L"StarRupture-ModLoader-AutoUpdate/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession)
		return {};

	// Cap individual operation timeouts so we never block startup indefinitely.
	// resolve=5s, connect=10s, send=30s, receive=30s
	WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 30000);

	INTERNET_PORT port = (uc.nPort != 0) ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
	HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
	if (!hConnect)
	{
		WinHttpCloseHandle(hSession);
		return {};
	}

	DWORD reqFlags = WINHTTP_FLAG_SECURE;
	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect, L"GET", path,
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
	if (!hRequest)
	{
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return {};
	}

	// Follow GitHub CDN redirects automatically
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
		&redirectPolicy, sizeof(redirectPolicy));

	if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		|| !WinHttpReceiveResponse(hRequest, nullptr))
	{
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
	if (statusCode != 200)
	{
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
			break;
		chunk.resize(bytesRead);
		body += chunk;
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
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
	// Find "key"
	std::string needle = std::string("\"") + key + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return {};

	// Skip past : and optional whitespace to the opening "
	size_t vstart = json.find('"', kpos + needle.size());
	if (vstart == std::string::npos)
		return {};
	++vstart; // skip the opening quote

	// Collect until closing unescaped "
	std::string value;
	for (size_t i = vstart; i < json.size(); ++i)
	{
		if (json[i] == '\\' && i + 1 < json.size())
		{
			value += json[i + 1]; // take escaped char literally
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

	// Skip : and whitespace
	size_t vpos = kpos + needle.size();
	while (vpos < json.size() && (json[vpos] == ' ' || json[vpos] == '\t'
		|| json[vpos] == '\r' || json[vpos] == '\n'
		|| json[vpos] == ':'))
		++vpos;

	if (vpos >= json.size() || !isdigit(static_cast<unsigned char>(json[vpos])))
		return fallback;

	return std::stoi(json.substr(vpos));
}

// Split the "plugins": [ ... ] array into individual {  } object strings.
// Works via brace-depth tracking; assumes no nested objects inside entries.
static std::vector<std::string> JsonExtractObjectArray(
	const std::string& json, const char* arrayKey)
{
	std::vector<std::string> results;

	std::string needle = std::string("\"") + arrayKey + "\"";
	size_t kpos = json.find(needle);
	if (kpos == std::string::npos)
		return results;

	// Find the opening [
	size_t apos = json.find('[', kpos + needle.size());
	if (apos == std::string::npos)
		return results;

	// Walk through the array collecting { ... } blobs
	size_t i = apos + 1;
	while (i < json.size())
	{
		// Skip whitespace / commas
		if (json[i] == ' ' || json[i] == '\t' ||
			json[i] == '\r' || json[i] == '\n' || json[i] == ',')
		{
			++i;
			continue;
		}

		if (json[i] == ']')
			break; // end of array

		if (json[i] == '{')
		{
			// Collect the entire { ... } block
			size_t start = i;
			int depth = 0;
			bool inString = false;
			while (i < json.size())
			{
				char c = json[i];
				if (inString)
				{
					if (c == '\\') { ++i; } // skip escaped char
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

		++i; // skip unexpected characters
	}

	return results;
}

// ===========================================================================
// Section E — Download a single plugin DLL
// ===========================================================================

// Downloads url into <pluginsDir>\<filename>.tmp then renames to the final path.
// Returns true on success.
static bool DownloadPlugin(const char* url,
	const wchar_t* pluginsDir,
	const wchar_t* filename)
{
	wchar_t tmpPath[MAX_PATH]{};
	wchar_t finalPath[MAX_PATH]{};
	swprintf_s(tmpPath, L"%s\\%s.tmp", pluginsDir, filename);
	swprintf_s(finalPath, L"%s\\%s", pluginsDir, filename);

	std::string body = HttpGet(url);
	if (body.empty())
		return false;

	// Write to .tmp
	HANDLE hFile = CreateFileW(tmpPath, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	DWORD written = 0;
	BOOL ok = WriteFile(hFile, body.data(), static_cast<DWORD>(body.size()),
		&written, nullptr);
	CloseHandle(hFile);

	if (!ok || written != static_cast<DWORD>(body.size()))
	{
		DeleteFileW(tmpPath);
		return false;
	}

	// Atomic rename: replace existing in one operation so the original is
	// never deleted unless the new file is successfully placed.
	if (!MoveFileExW(tmpPath, finalPath, MOVEFILE_REPLACE_EXISTING))
	{
		DeleteFileW(tmpPath);
		return false;
	}

	return true;
}

// ===========================================================================
// Section F — Orchestrator
// ===========================================================================

void ModLoader::RunAutoUpdate()
{
	AutoUpdateConfig cfg = ReadAutoUpdateConfig();

	if (!cfg.enabled)
	{
		Log::Info("[AutoUpdate] Disabled via modloader.ini - skipping");
		return;
	}

	if (cfg.manifestUrl[0] == '\0')
	{
		Log::Info("[AutoUpdate] No manifest URL configured (dev build?) - skipping");
		return;
	}

	Log::Info("[AutoUpdate] Fetching manifest: %s", cfg.manifestUrl);

	std::string manifest = HttpGet(cfg.manifestUrl);
	if (manifest.empty())
	{
		Log::Warn("[AutoUpdate] Manifest fetch failed - skipping (network unavailable?)");
		return;
	}

	// Gate on interface version
	int manifestIfaceVer = JsonExtractInt(manifest, "interface_version", -1);
	if (manifestIfaceVer != PLUGIN_INTERFACE_VERSION)
	{
		Log::Warn("[AutoUpdate] Manifest interface_version=%d, loader=%d - skipping "
			"(interface version mismatch; update the loader first)",
			manifestIfaceVer, PLUGIN_INTERFACE_VERSION);
		return;
	}

	std::string remoteBuildTag = JsonExtractString(manifest, "build_tag");
	if (remoteBuildTag.empty())
	{
		Log::Warn("[AutoUpdate] Manifest missing build_tag - skipping");
		return;
	}

	// Determine plugins directory
	wchar_t pluginsDir[MAX_PATH]{};
	GetModuleFileNameW(nullptr, pluginsDir, MAX_PATH);
	wchar_t* slash = wcsrchr(pluginsDir, L'\\');
	if (slash)
		wcscpy_s(slash + 1, static_cast<rsize_t>(MAX_PATH - static_cast<DWORD>(slash + 1 - pluginsDir)),
			L"Plugins");

	// Ensure Plugins directory exists (in case we're running before first load)
	CreateDirectoryW(pluginsDir, nullptr);

	// Check stored tag
	char storedTag[256]{};
	ReadStoredBuildTag(storedTag, sizeof(storedTag));

	auto pluginEntries = JsonExtractObjectArray(manifest, "plugins");

	bool tagsMatch = (strcmp(storedTag, remoteBuildTag.c_str()) == 0);
	if (tagsMatch)
	{
		// Tags match — but still verify each DLL is actually on disk.
		// If all present, nothing to do.
		bool allPresent = true;
		for (const auto& entry : pluginEntries)
		{
			std::string filename = JsonExtractString(entry, "filename");
			if (filename.empty()) continue;

			wchar_t wFilename[256]{};
			MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wFilename, 256);

			wchar_t dllPath[MAX_PATH]{};
			swprintf_s(dllPath, L"%s\\%s", pluginsDir, wFilename);

			if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES)
			{
				Log::Info("[AutoUpdate] Plugin '%s' missing on disk despite matching tag - will re-download",
					filename.c_str());
				allPresent = false;
				break;
			}
		}

		if (allPresent)
		{
			Log::Info("[AutoUpdate] Already up to date (%s)", storedTag);
			return;
		}
	}
	else
	{
		Log::Info("[AutoUpdate] Update available: [%s] -> [%s]",
			storedTag[0] ? storedTag : "<none>", remoteBuildTag.c_str());
	}

	// Download each plugin entry
	int downloaded = 0;
	int failed = 0;

	for (const auto& entry : pluginEntries)
	{
		std::string name = JsonExtractString(entry, "name");
		std::string filename = JsonExtractString(entry, "filename");
		std::string downloadUrl = JsonExtractString(entry, "download_url");

		if (filename.empty() || downloadUrl.empty())
		{
			Log::Warn("[AutoUpdate] Skipping malformed plugin entry (missing filename or url)");
			++failed;
			continue;
		}

		wchar_t wFilename[256]{};
		MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wFilename, 256);

		// If tags match and this file is present, skip it
		if (tagsMatch)
		{
			wchar_t dllPath[MAX_PATH]{};
			swprintf_s(dllPath, L"%s\\%s", pluginsDir, wFilename);
			if (GetFileAttributesW(dllPath) != INVALID_FILE_ATTRIBUTES)
				continue; // present — skip
		}

		Log::Info("[AutoUpdate] Downloading %s ...", name.empty() ? filename.c_str() : name.c_str());

		if (DownloadPlugin(downloadUrl.c_str(), pluginsDir, wFilename))
		{
			Log::Info("[AutoUpdate] %s -> OK", filename.c_str());
			++downloaded;
		}
		else
		{
			Log::Warn("[AutoUpdate] Failed to download %s - will keep existing if present",
				filename.c_str());
			++failed;
		}
	}

	// Persist the new build tag so subsequent boots skip the download pass
	if (failed == 0 && downloaded >= 0)
	{
		WriteStoredBuildTag(remoteBuildTag.c_str());
		Log::Info("[AutoUpdate] Complete: %d downloaded, build_tag stored as [%s]",
			downloaded, remoteBuildTag.c_str());
	}
	else
	{
		Log::Warn("[AutoUpdate] %d plugin(s) failed to download; build_tag NOT updated "
			"so the update will be retried next boot", failed);
	}
}
