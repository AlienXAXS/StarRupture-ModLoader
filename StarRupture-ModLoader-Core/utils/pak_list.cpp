#include "pak_list.h"
#include "../logging/logger.h"

#include <windows.h>
#include <vector>

namespace PakList
{
    static std::wstring g_summary;

    struct PakEntry
    {
        std::wstring relativePath; // relative to Content\Paks
        uint64_t     sizeBytes = 0;
    };

    // <exe dir>\..\..\Content\Paks  (exe lives in <game>\Binaries\Win64\)
    static std::wstring GetPaksRoot()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);

        // Strip "\StarRupture.exe", "\Win64", "\Binaries"
        for (int i = 0; i < 3; ++i)
        {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (!slash)
                return L"";
            *slash = L'\0';
        }

        std::wstring result = path;
        result += L"\\Content\\Paks";
        return result;
    }

    static void ScanRecursive(const std::wstring& root, const std::wstring& subDir,
                              std::vector<PakEntry>& out)
    {
        std::wstring dir = root;
        if (!subDir.empty())
            dir += L"\\" + subDir;

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
            return;

        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;

            std::wstring rel = subDir.empty() ? fd.cFileName : subDir + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                ScanRecursive(root, rel, out);
                continue;
            }

            const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
            if (!ext || _wcsicmp(ext, L".pak") != 0)
                continue;

            PakEntry entry;
            entry.relativePath = std::move(rel);
            entry.sizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out.push_back(std::move(entry));
        } while (FindNextFileW(find, &fd));

        FindClose(find);
    }

    void CaptureAndLog()
    {
        const std::wstring paksRoot = GetPaksRoot();
        if (paksRoot.empty())
        {
            ModLoaderLogger::LogWarn(L"[PakList] Could not resolve the Content\\Paks directory");
            return;
        }

        std::vector<PakEntry> paks;
        ScanRecursive(paksRoot, L"", paks);

        ModLoaderLogger::LogInfo(L"[PakList] %zu pak file(s) in %s:", paks.size(), paksRoot.c_str());

        std::wstring summary;
        wchar_t line[512]{};
        swprintf_s(line, L"Pak files (%zu found in Content\\Paks):\r\n", paks.size());
        summary += line;

        size_t modPaks = 0;
        for (const PakEntry& pak : paks)
        {
            // Anything not sitting directly in Content\Paks (e.g. ~mods\,
            // LogicMods\) is almost certainly a user-installed mod pak.
            const bool isModPak = pak.relativePath.find(L'\\') != std::wstring::npos;
            if (isModPak)
                ++modPaks;

            const double sizeMb = static_cast<double>(pak.sizeBytes) / (1024.0 * 1024.0);
            swprintf_s(line, L"  %s  (%.1f MB)%s",
                pak.relativePath.c_str(), sizeMb,
                isModPak ? L"  <-- mod pak" : L"");

            ModLoaderLogger::LogInfo(L"[PakList]   %s", line + 2);
            summary += line;
            summary += L"\r\n";
        }

        if (modPaks > 0)
        {
            swprintf_s(line,
                L"\r\n%zu pak(s) above look like user-installed mod paks. If the game\r\n"
                L"crashes, an out-of-date pak mod is the most common cause -- try\r\n"
                L"removing them before blaming the ModLoader or the game.\r\n",
                modPaks);
            summary += line;
            ModLoaderLogger::LogInfo(L"[PakList] %zu likely mod pak(s) detected", modPaks);
        }

        g_summary = std::move(summary);
    }

    const std::wstring& GetSummary()
    {
        return g_summary;
    }
}
