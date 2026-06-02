#include "version_check.h"
#include "globals.h"
#include "../logging/log.h"
#include "../UI/splash_window.h"

#include <windows.h>
#include <winver.h>
#include <vector>

#pragma comment(lib, "version.lib")

static constexpr wchar_t kRequiredVersionSuffix[] = L"CL-120148";

static bool CheckGameVersion(std::wstring& outActualVersion)
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    DWORD dummy = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(exePath, &dummy);
    if (infoSize == 0)
    {
        outActualVersion = L"<unavailable>";
        return false;
    }

    std::vector<BYTE> buf(infoSize);
    if (!GetFileVersionInfoW(exePath, 0, infoSize, buf.data()))
    {
        outActualVersion = L"<unreadable>";
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
        outActualVersion = L"<not found>";
        return false;
    }

    outActualVersion = productVersion;

    const size_t reqLen    = wcslen(kRequiredVersionSuffix);
    const size_t actualLen = outActualVersion.size();
    return actualLen >= reqLen
        && outActualVersion.compare(actualLen - reqLen, reqLen, kRequiredVersionSuffix) == 0;
}

bool VerifyGameVersion()
{
    std::wstring gameVersion;

#if _DEBUG
    const bool versionOk = true;
#else
    const bool versionOk = CheckGameVersion(gameVersion);
#endif

    if (!versionOk)
    {
        LogToFile::Error("[ModLoader] VERSION MISMATCH: expected suffix '%ls', got '%ls'",
         kRequiredVersionSuffix, gameVersion.c_str());
        LogToFile::Error("[ModLoader] Hook installation aborted -- update to the correct game build.");

#ifdef MODLOADER_CLIENT_BUILD
        Splash::SetErrorMode(false);
        for (int countdown = 5; countdown > 0; --countdown)
        {
            wchar_t msg[64];
            swprintf_s(msg, L"Invalid Game Version - Plugins will not be loaded (%d)", countdown);
            Splash::SetStatus(msg);
            Sleep(1000);
        }
        Splash::Close();
#endif

        return false;
    }

    LogToFile::Info("[ModLoader] Game version OK: %ls", gameVersion.c_str());
    return true;
}
