// updater.cpp -- see updater.h for the design overview.

#include "updater.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "autoupdate_log.h"
#include "miniz.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

// Compiled-in default manifest URL, injected by CI via
// /p:AutoUpdateManifestUrl=... (same define Core uses).  Empty on dev
// builds -> update check disabled at runtime.
#ifdef AUTOUPDATE_MANIFEST_URL
#  define UPDATER_DEFAULT_MANIFEST_URL AUTOUPDATE_MANIFEST_URL
#else
#  define UPDATER_DEFAULT_MANIFEST_URL ""
#endif

// Debug builds: exercise the full update pipeline even though CI never
// injects a manifest URL into local builds.  A hardcoded URL to the real
// GitHub release feed is used as a last-resort default, and the version
// comparison is bypassed so the latest release is always offered for
// (re-)download -- the download / install path is then identical to what
// release builds run.
#ifdef _DEBUG
#  define UPDATER_DEBUG_FORCE_UPDATE 0
#  define UPDATER_DEBUG_MANIFEST_URL \
       "https://github.com/AlienXAXS/StarRupture-ModLoader/releases/latest/download/manifest-client.json"
#else
#  define UPDATER_DEBUG_FORCE_UPDATE 0
#endif

namespace
{
    // Game's Binaries\Win64 directory, set once by RunUpdater().
    std::wstring g_gameDir;

    // =======================================================================
    // Path helpers
    // =======================================================================

    std::wstring GamePath(const wchar_t* suffix)
    {
        return g_gameDir + L"\\" + suffix;
    }

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool DirExists(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    // Creates every intermediate directory of a full file path (the last
    // component is assumed to be the file name and is not created).
    bool EnsureParentDirectories(const std::wstring& filePath)
    {
        const std::wstring& path = filePath;
        // Walk forward creating each directory level below the root.  Skip
        // the "C:\" drive prefix, or "\\server\share\" for UNC paths.
        size_t start = 3; // past "C:\"
        if (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\')
        {
            size_t afterServer = path.find(L'\\', 2);
            size_t afterShare = (afterServer == std::wstring::npos)
                                    ? std::wstring::npos
                                    : path.find(L'\\', afterServer + 1);
            if (afterShare == std::wstring::npos)
                return false;
            start = afterShare + 1;
        }
        size_t pos = path.find(L'\\', start);
        while (pos != std::wstring::npos)
        {
            std::wstring dir = path.substr(0, pos);
            if (!CreateDirectoryW(dir.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
            {
                if (!DirExists(dir))
                    return false;
            }
            pos = path.find(L'\\', pos + 1);
        }
        return true;
    }

    // =======================================================================
    // INI helpers
    // =======================================================================

    std::wstring IniReadString(const std::wstring& iniPath,
                               const wchar_t* section, const wchar_t* key)
    {
        wchar_t buf[512]{};
        GetPrivateProfileStringW(section, key, L"", buf, 512, iniPath.c_str());
        return buf;
    }

    bool IniWriteString(const std::wstring& iniPath,
                        const wchar_t* section, const wchar_t* key,
                        const wchar_t* value)
    {
        return WritePrivateProfileStringW(section, key, value, iniPath.c_str()) != 0;
    }

    // =======================================================================
    // Version parsing / comparison
    // =======================================================================

    struct ParsedVersion
    {
        int  parts[4] = { 0, 0, 0, 0 };
        bool valid = false;
        bool hasSuffix = false; // e.g. "-preview" / "-beta.1"
    };

    // Accepts "v1.15.4", "1.15.2.0", "v1.16.0-beta.1", etc.
    ParsedVersion ParseVersion(const char* s)
    {
        ParsedVersion v;
        if (!s || !*s)
            return v;

        const char* p = s;
        if (*p == 'v' || *p == 'V')
            ++p;

        int idx = 0;
        while (idx < 4)
        {
            if (*p < '0' || *p > '9')
                break;
            long val = 0;
            while (*p >= '0' && *p <= '9')
            {
                val = val * 10 + (*p - '0');
                if (val > 1000000) val = 1000000;
                ++p;
            }
            v.parts[idx++] = static_cast<int>(val);
            if (*p == '.')
                ++p;
            else
                break;
        }

        v.valid = (idx >= 2); // require at least major.minor
        v.hasSuffix = (*p == '-' || *p == '+');
        return v;
    }

    // Returns >0 if a is newer than b, 0 if equal, <0 if older (numeric only).
    int CompareVersions(const ParsedVersion& a, const ParsedVersion& b)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (a.parts[i] != b.parts[i])
                return a.parts[i] < b.parts[i] ? -1 : 1;
        }
        return 0;
    }

    // Reads the FileVersion (A.B.C.D) from a DLL's version resource on disk.
    // The DLL is NOT loaded as a module.  Returns an invalid ParsedVersion
    // on any failure.
    ParsedVersion GetDllFileVersion(const std::wstring& dllPath, std::string& outDisplay)
    {
        ParsedVersion v;
        outDisplay.clear();

        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(dllPath.c_str(), &handle);
        if (size == 0)
            return v;

        std::vector<char> data(size);
        if (!GetFileVersionInfoW(dllPath.c_str(), 0, size, data.data()))
            return v;

        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT ffiLen = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &ffiLen) ||
            !ffi || ffiLen < sizeof(VS_FIXEDFILEINFO))
            return v;

        v.parts[0] = HIWORD(ffi->dwFileVersionMS);
        v.parts[1] = LOWORD(ffi->dwFileVersionMS);
        v.parts[2] = HIWORD(ffi->dwFileVersionLS);
        v.parts[3] = LOWORD(ffi->dwFileVersionLS);
        v.valid = (v.parts[0] | v.parts[1] | v.parts[2] | v.parts[3]) != 0;

        char buf[64];
        sprintf_s(buf, "%d.%d.%d", v.parts[0], v.parts[1], v.parts[2]);
        outDisplay = buf;
        return v;
    }

    // =======================================================================
    // Minimal JSON string field extraction (flat manifest only)
    // =======================================================================

    std::string JsonExtractString(const std::string& json, const char* key)
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
                break;
            else
                value += json[i];
        }
        return value;
    }

    // =======================================================================
    // WinHTTP GET (HTTPS, follows redirects -- needed for GitHub CDN)
    // =======================================================================

    std::string FormatWin32Error(DWORD err)
    {
        char buf[512]{};
        HMODULE hWinHttp = GetModuleHandleW(L"winhttp.dll");
        DWORD n = 0;
        if (hWinHttp)
            n = FormatMessageA(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                               hWinHttp, err, 0, buf, sizeof(buf) - 1, nullptr);
        if (n == 0)
            n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
        while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
            buf[--n] = '\0';
        if (n == 0)
            return "unknown error " + std::to_string(err);
        return buf;
    }

    // Returns the response body, or empty on any error.  maxSize caps the
    // download so a bad server cannot exhaust memory.
    std::string HttpGet(const std::string& url, const char* label, size_t maxSize)
    {
        AutoUpdateLog::Info("HttpGet [%s]: %s", label, url.c_str());

        std::wstring wUrl = Widen(url);

        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256]{}, path[1024]{};
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 1024;
        if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &uc))
        {
            AutoUpdateLog::Error("HttpGet [%s]: invalid URL (%lu: %s)",
                                 label, GetLastError(), FormatWin32Error(GetLastError()).c_str());
            return {};
        }

        HINTERNET hSession = WinHttpOpen(L"StarRupture-ModLoader-Updater/1.0",
                                         WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
        if (!hSession)
        {
            AutoUpdateLog::Error("HttpGet [%s]: WinHttpOpen failed (%lu: %s)",
                                 label, GetLastError(), FormatWin32Error(GetLastError()).c_str());
            return {};
        }

        // resolve=5s, connect=10s, send=15s, receive=60s (release ZIP can be
        // several MB on slow connections).  These also bound how long an
        // offline machine stalls game startup: ~15s worst case.
        WinHttpSetTimeouts(hSession, 5000, 10000, 15000, 60000);

        std::string body;
        HINTERNET hConnect = nullptr, hRequest = nullptr;
        bool ok = false;

        do
        {
            INTERNET_PORT port = (uc.nPort != 0) ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
            hConnect = WinHttpConnect(hSession, host, port, 0);
            if (!hConnect)
            {
                AutoUpdateLog::Error("HttpGet [%s]: WinHttpConnect failed (%lu: %s)",
                                     label, GetLastError(), FormatWin32Error(GetLastError()).c_str());
                break;
            }

            hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
                                          nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
            if (!hRequest)
            {
                AutoUpdateLog::Error("HttpGet [%s]: WinHttpOpenRequest failed (%lu: %s)",
                                     label, GetLastError(), FormatWin32Error(GetLastError()).c_str());
                break;
            }

            DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                             &redirectPolicy, sizeof(redirectPolicy));

            if (!WinHttpSendRequest(hRequest, nullptr, 0, nullptr, 0, 0, 0) ||
                !WinHttpReceiveResponse(hRequest, nullptr))
            {
                AutoUpdateLog::Error("HttpGet [%s]: request failed (%lu: %s)",
                                     label, GetLastError(), FormatWin32Error(GetLastError()).c_str());
                break;
            }

            DWORD statusCode = 0, statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &statusCode, &statusSize, nullptr);
            if (statusCode != 200)
            {
                AutoUpdateLog::Error("HttpGet [%s]: server returned HTTP %lu", label, statusCode);
                break;
            }

            bool readOk = true;
            for (;;)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &available))
                {
                    AutoUpdateLog::Error("HttpGet [%s]: read failed (%lu: %s) after %zu bytes",
                                         label, GetLastError(),
                                         FormatWin32Error(GetLastError()).c_str(), body.size());
                    readOk = false;
                    break;
                }
                if (available == 0)
                    break;

                if (body.size() + available > maxSize)
                {
                    AutoUpdateLog::Error("HttpGet [%s]: response exceeds %zu byte limit -- aborting",
                                         label, maxSize);
                    readOk = false;
                    break;
                }

                std::string chunk(available, '\0');
                DWORD bytesRead = 0;
                if (!WinHttpReadData(hRequest, chunk.data(), available, &bytesRead))
                {
                    AutoUpdateLog::Error("HttpGet [%s]: read failed mid-stream (%lu: %s) after %zu bytes",
                                         label, GetLastError(),
                                         FormatWin32Error(GetLastError()).c_str(), body.size());
                    readOk = false;
                    break;
                }
                chunk.resize(bytesRead);
                body += chunk;
            }

            if (!readOk)
                break;

            AutoUpdateLog::Info("HttpGet [%s]: received %zu bytes", label, body.size());
            ok = true;
        } while (false);

        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);

        if (!ok)
            return {};
        return body;
    }

    // =======================================================================
    // Download + install
    // =======================================================================

    const wchar_t* kBackupSuffix = L".autoupdate.bak";

    // Rejects ZIP entry names that could escape the game directory, and
    // normalizes '/' to '\'.  Returns empty string when the entry is unsafe.
    std::wstring SanitizeZipEntryName(const char* name)
    {
        std::string n = name ? name : "";
        if (n.empty())
            return {};

        for (char& c : n)
            if (c == '/')
                c = '\\';

        // No absolute paths, drive letters, or parent traversal
        if (n[0] == '\\' || n.find(':') != std::string::npos ||
            n.find("..") != std::string::npos)
            return {};

        return Widen(n);
    }

    bool WriteBufferToFile(const std::wstring& path, const void* data, size_t size)
    {
        if (!EnsureParentDirectories(path))
            return false;

        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        DWORD written = 0;
        BOOL ok = WriteFile(hFile, data, static_cast<DWORD>(size), &written, nullptr);
        CloseHandle(hFile);
        return ok && written == static_cast<DWORD>(size);
    }

    struct ExtractedFile
    {
        std::wstring      relPath; // relative to the game directory
        std::vector<char> data;
    };

    struct AppliedFile
    {
        std::wstring target;  // final path in the game directory
        std::wstring backup;  // <target>.autoupdate.bak ("" if target was new)
        bool         written; // new file was placed at target
    };

    void RollbackInstall(std::vector<AppliedFile>& applied)
    {
        AutoUpdateLog::Error("Install: rolling back %zu file(s)", applied.size());
        for (auto it = applied.rbegin(); it != applied.rend(); ++it)
        {
            if (it->written)
                DeleteFileW(it->target.c_str());
            if (!it->backup.empty())
            {
                if (!MoveFileExW(it->backup.c_str(), it->target.c_str(), MOVEFILE_REPLACE_EXISTING))
                    AutoUpdateLog::Error("Install: ROLLBACK FAILED for '%ls' (error %lu) -- "
                                         "manual reinstall may be required",
                                         it->target.c_str(), GetLastError());
            }
        }
    }

    // Downloads the release ZIP, decompresses it entirely in memory, then
    // installs every file except dwmapi.dll with backup/rollback.  Returns
    // true only when every file has been placed successfully.
    bool DownloadAndInstall(const std::string& zipUrl, const std::string& remoteTag)
    {
        // ------------------------------------------------------------------
        // Phase 1: download (memory only; capped at 256 MB sanity limit)
        // ------------------------------------------------------------------
        std::string zipData = HttpGet(zipUrl, "release zip", 256u * 1024 * 1024);
        if (zipData.size() < 1024)
        {
            AutoUpdateLog::Error("Install: download failed or implausibly small (%zu bytes) -- aborting",
                                 zipData.size());
            return false;
        }

        // ------------------------------------------------------------------
        // Phase 2: validate + decompress the whole archive in memory.
        // Nothing on disk is touched until every entry extracted cleanly.
        // ------------------------------------------------------------------
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        {
            AutoUpdateLog::Error("Install: downloaded file is not a valid ZIP archive (miniz error: %s)",
                                 mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
            return false;
        }

        std::vector<ExtractedFile> files;
        bool sawCoreDll = false;
        bool failed = false;

        mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
        AutoUpdateLog::Info("Install: archive contains %u entries", fileCount);

        for (mz_uint i = 0; i < fileCount && !failed; ++i)
        {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip, i, &st))
            {
                AutoUpdateLog::Error("Install: could not stat ZIP entry %u -- aborting", i);
                failed = true;
                break;
            }

            if (mz_zip_reader_is_file_a_directory(&zip, i))
                continue;

            std::wstring relPath = SanitizeZipEntryName(st.m_filename);
            if (relPath.empty())
            {
                AutoUpdateLog::Error("Install: unsafe ZIP entry name '%s' -- aborting update",
                                     st.m_filename);
                failed = true;
                break;
            }

            if (_wcsicmp(relPath.c_str(), L"ModLoader\\StarRupture-ModLoader-Core.dll") == 0)
                sawCoreDll = true;

            size_t extractedSize = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &extractedSize, 0);
            if (!data)
            {
                AutoUpdateLog::Error("Install: extraction failed for '%s' (miniz error: %s) -- aborting",
                                     st.m_filename,
                                     mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
                failed = true;
                break;
            }

            if (extractedSize != static_cast<size_t>(st.m_uncomp_size))
            {
                AutoUpdateLog::Error("Install: size mismatch for '%s' (%zu vs %llu expected) -- aborting",
                                     st.m_filename, extractedSize,
                                     static_cast<unsigned long long>(st.m_uncomp_size));
                mz_free(data);
                failed = true;
                break;
            }

            ExtractedFile ef;
            ef.relPath = relPath;
            ef.data.assign(static_cast<char*>(data), static_cast<char*>(data) + extractedSize);
            mz_free(data);
            files.push_back(std::move(ef));

            AutoUpdateLog::Info("Install: extracted '%s' (%zu bytes)", st.m_filename, extractedSize);
        }

        mz_zip_reader_end(&zip);

        if (!failed && !sawCoreDll)
        {
            AutoUpdateLog::Error("Install: archive does not contain ModLoader\\StarRupture-ModLoader-Core.dll "
                                 "-- refusing to install a suspicious update");
            failed = true;
        }

        if (!failed && files.empty())
        {
            AutoUpdateLog::Error("Install: archive contained no usable files -- aborting");
            failed = true;
        }

        if (failed)
            return false;

        // ------------------------------------------------------------------
        // Phase 3: install with backup -> write -> delete-backups.
        // In-use binaries (the game's loaded dwmapi.dll, this running exe)
        // cannot be overwritten but CAN be renamed, so the backup step works
        // for them too: the old image keeps running from the renamed file
        // and the new file is picked up on the next launch.  Their backups
        // cannot be deleted while in use; the proxy cleans those next boot.
        // ------------------------------------------------------------------
        std::vector<AppliedFile> applied;
        applied.reserve(files.size());

        for (const ExtractedFile& ef : files)
        {
            std::wstring target = GamePath(ef.relPath.c_str());

            AppliedFile af;
            af.target = target;
            af.written = false;

            if (!EnsureParentDirectories(target))
            {
                AutoUpdateLog::Error("Install: could not create directories for '%ls' (error %lu)",
                                     ef.relPath.c_str(), GetLastError());
                failed = true;
            }
            else
            {
                if (FileExists(target))
                {
                    std::wstring backup = target + kBackupSuffix;
                    DeleteFileW(backup.c_str()); // stale backup from an old run
                    if (!MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING))
                    {
                        AutoUpdateLog::Error("Install: could not back up '%ls' (error %lu: %s) -- aborting",
                                             ef.relPath.c_str(), GetLastError(),
                                             FormatWin32Error(GetLastError()).c_str());
                        failed = true;
                    }
                    else
                    {
                        af.backup = backup;
                    }
                }
            }

            if (failed)
            {
                applied.push_back(af); // include for rollback of the backup rename
                break;
            }

            applied.push_back(af);

            if (!WriteBufferToFile(target, ef.data.data(), ef.data.size()))
            {
                AutoUpdateLog::Error("Install: could not write '%ls' (error %lu: %s) -- aborting",
                                     ef.relPath.c_str(), GetLastError(),
                                     FormatWin32Error(GetLastError()).c_str());
                failed = true;
                break;
            }
            applied.back().written = true;

            AutoUpdateLog::Info("Install: placed '%ls'", ef.relPath.c_str());
        }

        if (failed)
        {
            RollbackInstall(applied);
            AutoUpdateLog::Error("Install: update %s FAILED and was rolled back -- "
                                 "the previous version remains installed", remoteTag.c_str());
            return false;
        }

        // Success: remove backups.  Deleting the backup of this running exe
        // fails while it is running -- the proxy cleans that up on next boot.
        for (const AppliedFile& af : applied)
        {
            if (!af.backup.empty() && !DeleteFileW(af.backup.c_str()))
                AutoUpdateLog::Info("Install: backup '%ls' still in use -- cleaned up on next boot",
                                    af.backup.c_str());
        }

        AutoUpdateLog::Info("Install: update %s installed successfully (%zu files)",
                            remoteTag.c_str(), applied.size());
        return true;
    }
}

// ===========================================================================
// Orchestrator
// ===========================================================================

int RunUpdater(const wchar_t* gameDir)
{
    g_gameDir = gameDir;
    while (!g_gameDir.empty() && g_gameDir.back() == L'\\')
        g_gameDir.pop_back();

    AutoUpdateLog::Info("Updater: started (game dir: %ls)", g_gameDir.c_str());

    std::wstring modloaderIni = GamePath(L"ModLoader\\modloader.ini");
    std::wstring stateIni     = GamePath(L"ModLoader\\update_state.ini");

    // Respect the shared AutoUpdate kill switch.
    if (GetPrivateProfileIntW(L"AutoUpdate", L"Enabled", 1, modloaderIni.c_str()) == 0)
    {
        AutoUpdateLog::Info("Auto-update disabled via modloader.ini [AutoUpdate] Enabled=0 -- skipping");
        return UPDATER_EXIT_UP_TO_DATE;
    }

    // Manifest URL: modloader.ini override wins, else compiled-in default.
    std::string manifestUrl;
    {
        std::wstring override_ = IniReadString(modloaderIni, L"AutoUpdate", L"ManifestUrl");
        if (!override_.empty())
        {
            manifestUrl = Narrow(override_);
            AutoUpdateLog::Info("Manifest URL (from modloader.ini): %s", manifestUrl.c_str());
        }
        else
        {
            manifestUrl = UPDATER_DEFAULT_MANIFEST_URL;
            if (manifestUrl.empty())
            {
#if UPDATER_DEBUG_FORCE_UPDATE
                manifestUrl = UPDATER_DEBUG_MANIFEST_URL;
                AutoUpdateLog::Warn("DEBUG build: no compiled-in manifest URL -- using hardcoded debug URL: %s",
                                    manifestUrl.c_str());
#else
                AutoUpdateLog::Info("No manifest URL configured (dev / generic build) -- update check skipped");
                return UPDATER_EXIT_UP_TO_DATE;
#endif
            }
            else
            {
                AutoUpdateLog::Info("Manifest URL (compiled-in): %s", manifestUrl.c_str());
            }
        }
    }

    // ----------------------------------------------------------------------
    // Determine the installed version.
    // Primary:  file version resource of the Core DLL on disk (cannot lie
    //           about which binary is actually installed).
    // Fallback: update_state.ini BuildTag, then the compiled build tag.
    // ----------------------------------------------------------------------
    ParsedVersion localVer;
    std::string localDisplay;
    {
        std::wstring corePath = GamePath(L"ModLoader\\StarRupture-ModLoader-Core.dll");
        localVer = GetDllFileVersion(corePath, localDisplay);
        if (localVer.valid)
        {
            AutoUpdateLog::Info("Installed version (Core DLL file version): %s", localDisplay.c_str());
        }
        else
        {
            AutoUpdateLog::Warn("Could not read Core DLL file version -- falling back to update_state.ini");
            std::string storedTag = Narrow(IniReadString(stateIni, L"AutoUpdate", L"BuildTag"));
            localVer = ParseVersion(storedTag.c_str());
            localDisplay = storedTag;
            if (!localVer.valid)
            {
#ifdef MODLOADER_BUILD_TAG
                localVer = ParseVersion(MODLOADER_BUILD_TAG);
                localDisplay = MODLOADER_BUILD_TAG;
                AutoUpdateLog::Warn("update_state.ini has no usable BuildTag -- using compiled tag %s",
                                    MODLOADER_BUILD_TAG);
#endif
            }
            else
            {
                AutoUpdateLog::Info("Installed version (update_state.ini): %s", storedTag.c_str());
            }
        }
    }

    if (!localVer.valid)
    {
#if UPDATER_DEBUG_FORCE_UPDATE
        AutoUpdateLog::Warn("DEBUG build: installed version unknown -- continuing anyway");
        localDisplay = "unknown (debug)";
#else
        AutoUpdateLog::Error("Installed version could not be determined by any method -- "
                             "refusing to offer an update (failsafe)");
        return UPDATER_EXIT_UP_TO_DATE;
#endif
    }

    // ----------------------------------------------------------------------
    // Fetch and parse the remote manifest.
    // ----------------------------------------------------------------------
    std::string manifest = HttpGet(manifestUrl, "manifest", 1024 * 1024);
    if (manifest.empty())
    {
        AutoUpdateLog::Warn("Manifest fetch failed -- update check skipped this session");
        return UPDATER_EXIT_UP_TO_DATE;
    }

    std::string remoteTag = JsonExtractString(manifest, "build_tag");
    if (remoteTag.empty())
    {
        AutoUpdateLog::Error("Manifest has no 'build_tag' field -- update check skipped");
        return UPDATER_EXIT_UP_TO_DATE;
    }

    ParsedVersion remoteVer = ParseVersion(remoteTag.c_str());
    AutoUpdateLog::Info("Remote build_tag: %s", remoteTag.c_str());
    if (!remoteVer.valid)
    {
        AutoUpdateLog::Error("Remote build_tag '%s' is not a valid version -- update check skipped",
                             remoteTag.c_str());
        return UPDATER_EXIT_UP_TO_DATE;
    }

#if UPDATER_DEBUG_FORCE_UPDATE
    // Debug builds always offer the latest release so the whole
    // download -> install pipeline can be tested, regardless of what is
    // installed and of any earlier decline.
    if (CompareVersions(remoteVer, localVer) <= 0)
        AutoUpdateLog::Warn("DEBUG build: installed %s is not older than remote %s -- forcing update offer anyway",
                            localDisplay.c_str(), remoteTag.c_str());
    else
        AutoUpdateLog::Info("Update available: %s -> %s", localDisplay.c_str(), remoteTag.c_str());
#else
    if (CompareVersions(remoteVer, localVer) <= 0)
    {
        AutoUpdateLog::Info("Mod loader is up to date (installed %s, remote %s)",
                            localDisplay.c_str(), remoteTag.c_str());
        // Clear any stale declined marker so a future release prompts again.
        IniWriteString(stateIni, L"AutoUpdate", L"ProxyDeclinedTag", nullptr);
        return UPDATER_EXIT_UP_TO_DATE;
    }

    AutoUpdateLog::Info("Update available: %s -> %s", localDisplay.c_str(), remoteTag.c_str());

    // Don't nag every boot about a version the user already declined.
    std::string declinedTag = Narrow(IniReadString(stateIni, L"AutoUpdate", L"ProxyDeclinedTag"));
    if (!declinedTag.empty() && declinedTag == remoteTag)
    {
        AutoUpdateLog::Info("User previously declined %s -- not asking again", remoteTag.c_str());
        return UPDATER_EXIT_DECLINED;
    }
#endif

    // ----------------------------------------------------------------------
    // Ask the user.
    // ----------------------------------------------------------------------
    wchar_t prompt[512];
    swprintf_s(prompt,
               L"A new version of the StarRupture Mod Loader is available.\n\n"
               L"Installed version:  %S\n"
               L"Latest version:      %S\n\n"
               L"Install it now?  The game will continue loading with the new "
               L"version once the update completes.",
               localDisplay.c_str(), remoteTag.c_str());

    int answer = MessageBoxW(nullptr, prompt,
                             L"StarRupture Mod Loader -- Update Available",
                             MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
    if (answer != IDYES)
    {
        AutoUpdateLog::Info("User declined update %s", remoteTag.c_str());
        IniWriteString(stateIni, L"AutoUpdate", L"ProxyDeclinedTag", Widen(remoteTag).c_str());
        return UPDATER_EXIT_DECLINED;
    }

    AutoUpdateLog::Info("User accepted update %s -- downloading", remoteTag.c_str());

    // Derive the ZIP URL from the manifest URL: both live in the same
    // release, so replace the file name portion.
    std::string zipUrl;
    {
        size_t slash = manifestUrl.find_last_of('/');
        if (slash == std::string::npos)
        {
            AutoUpdateLog::Error("Manifest URL has no path separator -- cannot derive ZIP URL");
            return UPDATER_EXIT_FAILED;
        }
        zipUrl = manifestUrl.substr(0, slash + 1) +
                 "StarRupture-ModLoader-Client-" + remoteTag + ".zip";
    }

    if (DownloadAndInstall(zipUrl, remoteTag))
    {
        // Record the new tag and clear the declined marker.
        IniWriteString(stateIni, L"AutoUpdate", L"BuildTag", Widen(remoteTag).c_str());
        IniWriteString(stateIni, L"AutoUpdate", L"ProxyDeclinedTag", nullptr);

        MessageBoxW(nullptr,
                    L"The mod loader has been updated successfully.\n\n"
                    L"The game will now continue loading with the new version.\n\n"
                    L"One small file finishes updating the next time you "
                    L"launch the game -- no action needed.",
                    L"StarRupture Mod Loader -- Update Installed",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        return UPDATER_EXIT_UPDATED;
    }

    AutoUpdateLog::Error("Update download/install failed -- the previous install was restored. "
                         "See messages above for the cause.");
    MessageBoxW(nullptr,
                L"The mod loader update could not be installed.\n\n"
                L"Your current installation has been restored and will keep "
                L"working.  See ModLoader\\Logs\\AutoUpdate.log for details.",
                L"StarRupture Mod Loader -- Update Failed",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    return UPDATER_EXIT_FAILED;
}

#else // !MODLOADER_CLIENT_BUILD

int RunUpdater(const wchar_t*)
{
    return UPDATER_EXIT_UP_TO_DATE;
}

#endif
