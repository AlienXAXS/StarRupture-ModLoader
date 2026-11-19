#include "unknown_build.h"

#ifdef MODLOADER_CLIENT_BUILD

#include <windows.h>

#include <atomic>

namespace UnknownBuild
{
    namespace
    {
        constexpr const wchar_t* kSection = L"UnknownBuild";

        // Set by Arm, read by the UI and the report lines for the rest of the
        // process. s_open is the only one EndSession flips.
        std::atomic<bool>     s_active{ false };
        std::atomic<bool>     s_open{ false };
        std::atomic<uint32_t> s_runningCL{ 0 };
        std::atomic<uint32_t> s_newestCL{ 0 };

        // Written in Arm before s_open is set, read in EndSession after the
        // exchange that clears it, so the two never overlap.
        std::wstring s_iniPath;

        std::wstring ToWide(const char* text)
        {
            if (!text || !*text)
                return std::wstring();
            const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
            if (n <= 1)
                return std::wstring();
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
            out.resize(static_cast<size_t>(n) - 1);
            return out;
        }

        std::wstring ReadSession(const std::wstring& iniPath)
        {
            wchar_t buf[128]{};
            GetPrivateProfileStringW(kSection, L"Session", L"", buf, 128, iniPath.c_str());
            return buf;
        }
    }

    std::wstring SessionId(uint32_t cl, uint32_t imageStamp, uint32_t imageSize, const char* loaderBuildTag)
    {
        wchar_t head[64]{};
        swprintf_s(head, L"%u:%08X:%X:", cl, imageStamp, imageSize);
        return std::wstring(head) + ToWide(loaderBuildTag);
    }

    Inputs ReadInputs(const std::wstring& iniPath, uint32_t runningCL, const std::wstring& thisSession)
    {
        Inputs in;
        in.runningCL   = runningCL;
        in.enabled     = GetPrivateProfileIntW(kSection, L"Enabled", 1, iniPath.c_str()) != 0;
        in.retry       = GetPrivateProfileIntW(kSection, L"Retry", 0, iniPath.c_str()) == 1;
        in.lastSession = ReadSession(iniPath);
        in.thisSession = thisSession;
        return in;
    }

    Decision Decide(const Inputs& in)
    {
        if (in.runningCL == 0)
            return Decision::NoCL;
        if (!in.enabled)
            return Decision::SwitchedOff;
        if (!in.lastSession.empty() && in.lastSession == in.thisSession && !in.retry)
            return Decision::Guarded;
        return Decision::Fallback;
    }

    uint32_t NewestKnown(const std::vector<uint32_t>& cls)
    {
        uint32_t newest = 0;
        for (const uint32_t cl : cls)
            if (cl > newest)
                newest = cl;
        return newest;
    }

    bool Arm(const std::wstring& iniPath, const std::wstring& sessionId, uint32_t runningCL, uint32_t newestCL)
    {
        // Retry goes first: if a later write failed, a Retry=1 left standing
        // would turn a crash loop into a retry loop.
        if (!WritePrivateProfileStringW(kSection, L"Retry", nullptr, iniPath.c_str()))
            return false;
        if (!WritePrivateProfileStringW(kSection, L"Session", sessionId.c_str(), iniPath.c_str()))
            return false;
        if (ReadSession(iniPath) != sessionId)
            return false;

        s_iniPath = iniPath;
        s_runningCL.store(runningCL);
        s_newestCL.store(newestCL);
        s_active.store(true);
        s_open.store(true);
        return true;
    }

    bool EndSession()
    {
        if (!s_open.exchange(false))
            return false;
        return WritePrivateProfileStringW(kSection, L"Session", nullptr, s_iniPath.c_str()) != FALSE;
    }

    bool     Active()     { return s_active.load(); }
    uint32_t RunningCL()  { return s_runningCL.load(); }
    uint32_t NewestCL()   { return s_newestCL.load(); }

    std::string NoticeText(uint32_t newestCL)
    {
        return "This game build is not in the ModLoader's version object map yet. "
            "The ModLoader is running with the values of the newest build it knows (CL-" + std::to_string(newestCL) +
            ") and checked each one against your game before using it. Anything that did not check out is "
            "switched off, so some plugins or features may be off. When the version map is updated for your "
            "build, the ModLoader picks that up by itself at a later start. If the game crashes or stops "
            "responding on this build, the next start runs without mods and tells you how to try again.";
    }

    std::string ReportLine(uint32_t runningCL, uint32_t newestCL)
    {
        return "CL-" + std::to_string(runningCL) + " is not in the version object map; running on checked values from CL-" +
            std::to_string(newestCL) + ", the newest known build";
    }

    std::wstring GuardDetails(const std::wstring& gameVersion, const char* loaderBuildTag)
    {
        return L"Your game build:       " + gameVersion + L"\r\n"
            L"ModLoader version:     " + ToWide(loaderBuildTag) + L"\r\n"
            L"\r\n"
            L"To try with mods again: in ModLoader\\modloader.ini, under [UnknownBuild], add the line Retry=1, "
            L"then start the game.";
    }

    std::wstring PreflightNote(uint32_t runningCL, uint32_t newestCL)
    {
        return L"This game build (CL-" + std::to_wstring(runningCL) + L") is not in the ModLoader's version object map yet. "
            L"The ModLoader tried the newest build it knows (CL-" + std::to_wstring(newestCL) +
            L"), and the patterns below did not check out on your build.\r\n\r\n";
    }
}

#endif // MODLOADER_CLIENT_BUILD
