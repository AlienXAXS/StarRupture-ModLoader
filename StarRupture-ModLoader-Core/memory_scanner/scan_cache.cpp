#include "memory_scanner/scan_cache.h"
#include "memory_scanner/image_info.h"
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

    // The section for the running exe: the ProductVersion the caller read,
    // with the main module's PE timestamp and image size.
    std::wstring SectionFor(const std::wstring& gameVersion)
    {
        const Scanner::ImageInfo image = Scanner::GetImageInfo(GetModuleHandleW(nullptr));
        return ScanCache::SectionName(gameVersion, image.stamp, static_cast<uint32_t>(image.size));
    }

    // The cache file is keyed by exe section. If the game has updated since the
    // file was written, every entry in it is for a stale build and would just
    // churn through "stale, rescan, overwrite" on every lookup -- so wipe the
    // whole file and start fresh for the current exe instead of letting
    // old-build sections accumulate forever.
    void DiscardCacheIfVersionChanged(const std::wstring& section)
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
        for (const wchar_t* name = sectionNames; *name; name += wcslen(name) + 1)
        {
            hasSections = true;
            if (section == name)
            {
                hasCurrentVersion = true;
                break;
            }
        }

        if (hasSections && !hasCurrentVersion)
        {
            ModLoaderLogger::LogInfo(L"[ScanCache] No section for '%ls' (another game build, or a cache an older loader wrote) -- discarding the scan cache",
                section.c_str());

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
    bool TryGet(const std::wstring& gameVersion, const std::string& pattern, Entry& out)
    {
        out = Entry{};

        if (gameVersion.empty() || pattern.empty())
            return false;

        const std::wstring section = SectionFor(gameVersion);
        DiscardCacheIfVersionChanged(section);

        const std::wstring cachePath = GetCacheFilePath();
        const std::wstring key       = HashPattern(pattern);

        wchar_t value[64]{};
        const DWORD len = GetPrivateProfileStringW(section.c_str(), key.c_str(), L"",
                                                    value, static_cast<DWORD>(std::size(value)), cachePath.c_str());
        if (len == 0)
            return false;

        // Value format is "0xOFFSET" or "0xOFFSET,COUNT". The bare form is what
        // every entry written before match counting existed looks like, and it
        // parses as count 0 = unknown, which forces a re-scan for anyone who
        // needs the uniqueness verdict. Nothing has to migrate the file.
        wchar_t* end = nullptr;
        const unsigned long long parsed = wcstoull(value, &end, 16);
        if (end == value)
            return false;

        out.offset = static_cast<uintptr_t>(parsed);

        if (end && *end == L',')
        {
            wchar_t* countEnd = nullptr;
            const unsigned long long count = wcstoull(end + 1, &countEnd, 10);
            if (countEnd != end + 1)
                out.matchCount = static_cast<uint32_t>(count);
        }

        return true;
    }

    void Store(const std::wstring& gameVersion, const std::string& pattern, const Entry& entry)
    {
        if (gameVersion.empty() || pattern.empty())
            return;

        const std::wstring section = SectionFor(gameVersion);
        DiscardCacheIfVersionChanged(section);

        const std::wstring cachePath = GetCacheFilePath();
        const std::wstring key       = HashPattern(pattern);

        // Don't let a "where is it" write (matchCount 0) clobber a count an
        // earlier strict resolve established for the same pattern. Losing the
        // count is not wrong, just slow -- but it is slow on every launch, and
        // the two entry points run in an order nobody controls.
        Entry merged = entry;
        if (merged.matchCount == 0)
        {
            Entry existing;
            if (TryGet(gameVersion, pattern, existing) && existing.offset == merged.offset)
                merged.matchCount = existing.matchCount;
        }

        wchar_t value[48]{};
        if (merged.matchCount != 0)
        {
            swprintf_s(value, L"0x%llX,%lu",
                static_cast<unsigned long long>(merged.offset),
                static_cast<unsigned long>(merged.matchCount));
        }
        else
        {
            swprintf_s(value, L"0x%llX", static_cast<unsigned long long>(merged.offset));
        }

        if (WritePrivateProfileStringW(section.c_str(), key.c_str(), value, cachePath.c_str()))
        {
            ModLoaderLogger::LogTrace(L"[ScanCache] Stored [%ls] under build '%ls': %ls",
                key.c_str(), section.c_str(), value);
        }
        else
        {
            ModLoaderLogger::LogWarn(L"[ScanCache] Failed to write cache file '%ls' (error %lu)",
                cachePath.c_str(), GetLastError());
        }
    }
}
