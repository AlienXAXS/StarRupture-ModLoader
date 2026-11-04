#include "version_check.h"
#include "globals.h"
#include "../logging/log.h"
#include "memory_scanner/version_map.h"

#include <windows.h>
#include <winver.h>
#include <vector>

#pragma comment(lib, "version.lib")

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

// Reads the raw ProductVersion string from the game executable's version info.
// Returns false (with a placeholder string) if it could not be read.
static bool ReadProductVersion(std::wstring& outVersion)
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    DWORD dummy = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(exePath, &dummy);
    if (infoSize == 0)
    {
        outVersion = L"<unavailable>";
        return false;
    }

    std::vector<BYTE> buf(infoSize);
    if (!GetFileVersionInfoW(exePath, 0, infoSize, buf.data()))
    {
        outVersion = L"<unreadable>";
        return false;
    }

    struct LangCodepage { WORD lang; WORD codepage; };
    LangCodepage* translations = nullptr;
    UINT cbTranslations = 0;
    VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
             reinterpret_cast<LPVOID*>(&translations), &cbTranslations);

    wchar_t query[64]{};
    if (translations && cbTranslations >= sizeof(LangCodepage))
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                   translations[0].lang, translations[0].codepage);
    else
        wcscpy_s(query, L"\\StringFileInfo\\040904b0\\ProductVersion");

    wchar_t* productVersion = nullptr;
    UINT versionLen = 0;
    if (!VerQueryValueW(buf.data(), query,
        reinterpret_cast<LPVOID*>(&productVersion), &versionLen)
        || !productVersion || versionLen == 0)
    {
        outVersion = L"<not found>";
        return false;
    }

    outVersion = productVersion;
    return true;
}

static bool CheckGameVersion(std::wstring& outActualVersion)
{
    if (!ReadProductVersion(outActualVersion))
        return false;

    return VersionMap::IsSupported(outActualVersion);
}

// "...-CL-127004 or ...-CL-128786": the builds the gate lets through.
static std::wstring SupportedBuildsText()
{
    std::wstring text;
    for (const uint32_t cl : VersionMap::SupportedBuilds())
        text += (text.empty() ? L"...-CL-" : L" or ...-CL-") + std::to_wstring(cl);
    return text;
}

std::wstring GetGameVersionString()
{
    std::wstring version;
    if (!ReadProductVersion(version))
        return std::wstring();
    return version;
}

bool VerifyGameVersion(std::wstring* outDetails)
{
    std::wstring gameVersion;

#if _DEBUG
    const bool versionOk = true;
#else
    const bool versionOk = CheckGameVersion(gameVersion);
#endif

    if (!versionOk)
    {
        LogToFile::Error("[ModLoader] VERSION MISMATCH: expected '%ls', got '%ls'",
         SupportedBuildsText().c_str(), gameVersion.c_str());
        LogToFile::Error("[ModLoader] Hook installation aborted -- update to the correct game build.");

        if (outDetails)
        {
            // CRLF: the crash dialog's edit control does not render bare '\n'.
            const wchar_t* detected = gameVersion.empty() ? L"<unknown>" : gameVersion.c_str();
            wchar_t buildTag[128]{};
            MultiByteToWideChar(CP_UTF8, 0, MODLOADER_BUILD_TAG, -1, buildTag, 128);

            *outDetails =
                std::wstring(L"Your game build:       ") + detected + L"\r\n"
                L"ModLoader expects:     " + SupportedBuildsText() + L"\r\n"
                L"ModLoader version:     " + buildTag + L"\r\n"
                L"\r\n"
                L"No hooks were installed and no plugins were loaded.";
        }

        return false;
    }

    LogToFile::Info("[ModLoader] Game version OK: %ls", gameVersion.c_str());
    return true;
}
