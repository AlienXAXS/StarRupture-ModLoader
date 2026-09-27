#include "pch.h"
#include "framegen_pause.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "logging/logger.h"
#include "../engine_exec/engine_exec.h"
#include <string>

namespace Hooks::FrameGenPause
{
    static const wchar_t* const kCVarName = L"r.Streamline.DLSSG.Enable";

    // Last state Tick() was called with, so it can tell a change from a
    // repeat and only touch the cvar once per transition.
    static bool s_lastWantPaused = false;

    // Value r.Streamline.DLSSG.Enable had when we last paused it. -1 means
    // "nothing to restore" -- either we haven't paused, or the read that
    // should have captured it failed.
    static int s_savedValue = -1;

    // A bare cvar name run through Exec logs "r.Streamline.DLSSG.Enable = "N""
    // (IConsoleManager::ProcessUserConsoleInput writes that through the same
    // FOutputDevice EngineExec captures) -- pull the first digit out of it.
    static bool ReadCurrentValue(int& outValue)
    {
        std::wstring result;
        if (Hooks::EngineExec::Execute(kCVarName, result) != Hooks::EngineExec::Result::Handled)
            return false;

        for (wchar_t c : result)
        {
            if (c >= L'0' && c <= L'9')
            {
                outValue = c - L'0';
                return true;
            }
        }
        return false;
    }

    static void WriteValue(int value)
    {
        wchar_t cmd[64];
        swprintf_s(cmd, L"%s %d", kCVarName, value);
        std::wstring result;
        Hooks::EngineExec::Execute(cmd, result);
    }

    void Tick(bool wantPaused)
    {
        if (wantPaused == s_lastWantPaused)
            return; // only act on a change
        s_lastWantPaused = wantPaused;

        if (wantPaused)
        {
            int current = 0;
            if (ReadCurrentValue(current))
            {
                s_savedValue = current;
                WriteValue(0);
                ModLoaderLogger::LogInfo(
                    L"[FrameGenPause] Overlay window opened -- disabling DLSS-G "
                    L"(%s was %d)", kCVarName, current);
            }
            else
            {
                s_savedValue = -1;
                ModLoaderLogger::LogWarn(
                    L"[FrameGenPause] Could not read %s -- leaving DLSS-G alone", kCVarName);
            }
        }
        else
        {
            if (s_savedValue >= 0)
            {
                WriteValue(s_savedValue);
                ModLoaderLogger::LogInfo(
                    L"[FrameGenPause] Last overlay window closed -- restoring %s to %d",
                    kCVarName, s_savedValue);
                s_savedValue = -1;
            }
        }
    }
}

#endif // MODLOADER_CLIENT_BUILD
