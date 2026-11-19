#include "version_check.h"
#include "globals.h"
#include "../logging/log.h"
#include "memory_scanner/version_map.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "startup_utils.h"
#include "unknown_build.h"
#endif

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

GameVersionVerdict VerifyGameVersion(std::wstring* outDetails)
{
    std::wstring gameVersion;

#if _DEBUG
    const bool versionOk = true;
#else
    const bool versionOk = CheckGameVersion(gameVersion);
#endif

    if (versionOk)
    {
        LogToFile::Info("[ModLoader] Game version OK: %ls", gameVersion.c_str());
        return GameVersionVerdict::Known;
    }

    wchar_t buildTag[128]{};
    MultiByteToWideChar(CP_UTF8, 0, MODLOADER_BUILD_TAG, -1, buildTag, 128);

    // Why an unknown build is refused instead of run on checked values, when it is
    // one of the two reasons the player can do something about.
    std::wstring note;
    bool switchedOff = false;

#ifdef MODLOADER_CLIENT_BUILD
    // A build the version object map has no rows for: try the newest known
    // build's values, which every lookup checks against this game anyway. The
    // server stays on its exact build.
    const VersionMap::Exe exe = VersionMap::RunningExe();
    const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");
    const std::wstring session = UnknownBuild::SessionId(exe.cl, exe.imageStamp, exe.imageSize, MODLOADER_BUILD_TAG);
    const UnknownBuild::Inputs inputs = UnknownBuild::ReadInputs(iniPath, exe.cl, session);
    const UnknownBuild::Decision decision = UnknownBuild::Decide(inputs);

    if (decision == UnknownBuild::Decision::Fallback || decision == UnknownBuild::Decision::Guarded)
    {
        const uint32_t newest = UnknownBuild::NewestKnown(VersionMap::SupportedBuilds());
        LogToFile::Warn("[ModLoader] Unknown game build: '%ls' (CL-%u) is not in the version object map (it has %ls)",
            gameVersion.c_str(), exe.cl, SupportedBuildsText().c_str());

        if (decision == UnknownBuild::Decision::Guarded)
        {
            LogToFile::Warn("[ModLoader] The last session on this build with this ModLoader did not end cleanly (crash, "
                "hang or forced close), so mods stay off this time. To try again, set Retry=1 under [UnknownBuild] "
                "in ModLoader\\modloader.ini");
            if (outDetails)
                *outDetails = UnknownBuild::GuardDetails(gameVersion, MODLOADER_BUILD_TAG);
            return GameVersionVerdict::Guarded;
        }

        LogToFile::Warn("[ModLoader] Trying the newest known build's values (CL-%u): each hook, address and field is "
            "checked against this game before use, and anything that does not check out stays off", newest);

        // The marker goes in before anything is patched; with no marker there is no guard, so no fallback.
        if (UnknownBuild::Arm(iniPath, session, exe.cl, newest))
        {
            LogToFile::Info("[ModLoader] Unknown-build session marked ([UnknownBuild] Session=%ls in modloader.ini); "
                "a clean exit clears it", session.c_str());
            if (inputs.retry)
                LogToFile::Info("[ModLoader] [UnknownBuild] Retry=1: trying again after an unclean session; the switch is cleared");
            return GameVersionVerdict::UnknownFallback;
        }

        LogToFile::Error("[ModLoader] Could not write the unknown-build session marker to modloader.ini, so a crash on "
            "this build could not be caught next time; not trying the newest known values");
        note = L"The ModLoader could not write ModLoader\\modloader.ini, which it needs to try an unknown game build safely.";
    }
    else if (decision == UnknownBuild::Decision::SwitchedOff)
    {
        switchedOff = true;
        note = L"Unknown game builds are switched off in ModLoader\\modloader.ini ([UnknownBuild] Enabled=0).";
    }
#endif

    LogToFile::Error("[ModLoader] VERSION MISMATCH: expected '%ls', got '%ls'",
     SupportedBuildsText().c_str(), gameVersion.c_str());
    LogToFile::Error("[ModLoader] Hook installation aborted -- update to the correct game build.");
    if (switchedOff)
        LogToFile::Info("[ModLoader] Unknown game builds are switched off ([UnknownBuild] Enabled=0 in modloader.ini)");

    if (outDetails)
    {
        // CRLF: the crash dialog's edit control does not render bare '\n'.
        const wchar_t* detected = gameVersion.empty() ? L"<unknown>" : gameVersion.c_str();

        *outDetails =
            std::wstring(L"Your game build:       ") + detected + L"\r\n"
            L"ModLoader expects:     " + SupportedBuildsText() + L"\r\n"
            L"ModLoader version:     " + buildTag + L"\r\n"
            L"\r\n" +
            (note.empty() ? std::wstring() : note + L"\r\n\r\n") +
            L"No hooks were installed and no plugins were loaded.";
    }

    return GameVersionVerdict::Refused;
}
