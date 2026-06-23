#include "memory_scanner/scan_cache.h"
#include "core/startup_utils.h"
#include "logging/logger.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
    const wchar_t* kCacheFileName = L"scan_cache.ini";

    std::wstring GetCacheFilePath()
    {
        return GetModLoaderDirPath(kCacheFileName);
    }

    // FNV-1a 64-bit -- turns an arbitrary-length AOB string ("48 8B ?? ?? ...")
    // into a short, fixed-width, INI-key-safe hex string.
    std::wstring HashPattern(const std::string& pattern)
    {
        uint64_t hash = 1469598103934665603ULL; // FNV offset basis
        for (unsigned char c : pattern)
        {
            hash ^= c;
            hash *= 1099511628211ULL; // FNV prime
        }

        wchar_t buf[20]{};
        swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(hash));
        return buf;
    }

    // The cache file is keyed by game version section. If the game has updated
    // since the file was written, every entry in it is for a stale build and
    // would just churn through "stale, rescan, overwrite" on every lookup --
    // so wipe the whole file and start fresh for the current version instead
    // of letting old-version sections accumulate forever.
    void DiscardCacheIfVersionChanged(const std::wstring& gameVersion)
    {
        static bool s_checked = false;
        if (s_checked)
            return;
        s_checked = true;

        const std::wstring cachePath = GetCacheFilePath();
        if (GetFileAttributesW(cachePath.c_str()) == INVALID_FILE_ATTRIBUTES)
            return; // No cache file yet -- nothing to discard.

        wchar_t sectionNames[2048]{};
        GetPrivateProfileSectionNamesW(sectionNames, static_cast<DWORD>(std::size(sectionNames)), cachePath.c_str());

        bool hasSections      = false;
        bool hasCurrentVersion = false;
        for (const wchar_t* section = sectionNames; *section; section += wcslen(section) + 1)
        {
            hasSections = true;
            if (gameVersion == section)
            {
                hasCurrentVersion = true;
                break;
            }
        }

        if (hasSections && !hasCurrentVersion)
        {
            ModLoaderLogger::LogInfo(L"[ScanCache] Game version changed (cache has no section for '%ls') -- discarding stale scan cache",
                gameVersion.c_str());

            if (!DeleteFileW(cachePath.c_str()))
            {
                ModLoaderLogger::LogWarn(L"[ScanCache] Failed to delete stale cache file '%ls' (error %lu)",
                    cachePath.c_str(), GetLastError());
            }
        }
    }
}

namespace ScanCache
{
    bool TryGetOffset(const std::wstring& gameVersion, const std::string& pattern, uintptr_t& outOffset)
    {
        if (gameVersion.empty() || pattern.empty())
            return false;

        DiscardCacheIfVersionChanged(gameVersion);

        const std::wstring cachePath = GetCacheFilePath();
        const std::wstring key       = HashPattern(pattern);

        wchar_t value[64]{};
        const DWORD len = GetPrivateProfileStringW(gameVersion.c_str(), key.c_str(), L"",
                                                    value, static_cast<DWORD>(std::size(value)), cachePath.c_str());
        if (len == 0)
            return false;

        wchar_t* end = nullptr;
        const unsigned long long parsed = wcstoull(value, &end, 16);
        if (end == value)
            return false;

        outOffset = static_cast<uintptr_t>(parsed);
        return true;
    }

    void StoreOffset(const std::wstring& gameVersion, const std::string& pattern, uintptr_t offset)
    {
        if (gameVersion.empty() || pattern.empty())
            return;

        DiscardCacheIfVersionChanged(gameVersion);

        const std::wstring cachePath = GetCacheFilePath();
        const std::wstring key       = HashPattern(pattern);

        wchar_t value[32]{};
        swprintf_s(value, L"0x%llX", static_cast<unsigned long long>(offset));

        if (WritePrivateProfileStringW(gameVersion.c_str(), key.c_str(), value, cachePath.c_str()))
        {
            ModLoaderLogger::LogDebug(L"[ScanCache] Stored offset for pattern hash [%ls] under version '%ls': %ls",
                key.c_str(), gameVersion.c_str(), value);
        }
        else
        {
            ModLoaderLogger::LogWarn(L"[ScanCache] Failed to write cache file '%ls' (error %lu)",
                cachePath.c_str(), GetLastError());
        }
    }
}
