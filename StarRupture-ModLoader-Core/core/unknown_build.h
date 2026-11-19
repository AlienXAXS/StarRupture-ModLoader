#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// UnknownBuild
//
// A game build the version object map has no rows for. Instead of switching
// itself off, the client runs on the newest known build's values: every key is
// still checked against the running game (VersionMap::Select and PickField),
// and anything that does not check out stays unresolved, so only the plugins
// or features that need it go off. See VerifyGameVersion (core/version_check.cpp)
// for where this is decided.
//
// This module holds the parts that can be tested on their own: the decision,
// the crash guard in modloader.ini, and the text the player sees.
//
//     [UnknownBuild]
//     Enabled=1     ; 0 = refuse unknown builds, as before
//     Retry=1       ; try again after a session that did not end cleanly
//     Session=...   ; written by the ModLoader while a session runs
//
// The crash guard: Arm writes Session before anything is hooked, and
// EndSession deletes it on a clean exit. A Session left behind by this same
// build and loader means the last try did not end cleanly, so the next start
// runs without mods until Retry=1 is set.
//
// Only <windows.h> and std, and no logging (the callers log), so the tests and
// the UI preview build it as it is.
// ---------------------------------------------------------------------------
namespace UnknownBuild
{
    enum class Decision
    {
        Fallback,      // run on the newest known build's values
        Guarded,       // the last session on this build did not end cleanly
        NoCL,          // the ProductVersion has no CL-<n>, so there is nothing to compare
        SwitchedOff,   // [UnknownBuild] Enabled=0
    };

    struct Inputs
    {
        uint32_t     runningCL = 0;
        bool         enabled   = true;    // [UnknownBuild] Enabled, default 1
        bool         retry     = false;   // [UnknownBuild] Retry=1
        std::wstring lastSession;         // [UnknownBuild] Session, empty when absent
        std::wstring thisSession;
    };

    // "130045:1A2B3C4D:F4A3000:1.25.1": the CL, the image stamp and size, the
    // loader build. A game patch or another loader build gives another id.
    std::wstring SessionId(uint32_t cl, uint32_t imageStamp, uint32_t imageSize, const char* loaderBuildTag);

    Inputs ReadInputs(const std::wstring& iniPath, uint32_t runningCL, const std::wstring& thisSession);

    // NoCL, then SwitchedOff, then Guarded, otherwise Fallback.
    Decision Decide(const Inputs& in);

    // The highest CL in the list, 0 when it is empty. Build names play no part.
    uint32_t NewestKnown(const std::vector<uint32_t>& cls);

    // Starts the session: clears Retry, writes Session and reads it back, then
    // marks the session active. False, with nothing marked, when the ini could
    // not be written; the caller then refuses the build rather than run with
    // no guard.
    bool Arm(const std::wstring& iniPath, const std::wstring& sessionId, uint32_t runningCL, uint32_t newestCL);

    // Ends the session cleanly: deletes Session. True when it cleared the
    // marker, false when no session was open (always so on a known build, and
    // it touches no file then).
    bool EndSession();

    // A fallback session runs in this process; stays true after EndSession.
    bool     Active();
    uint32_t RunningCL();
    uint32_t NewestCL();

    // Overlay body, UTF-8. The heading above it names the running CL.
    std::string  NoticeText(uint32_t newestCL);
    // One line, no label: "Unknown build: " goes in front where it is shown.
    std::string  ReportLine(uint32_t runningCL, uint32_t newestCL);
    // Details box of the guard dialog (CRLF).
    std::wstring GuardDetails(const std::wstring& gameVersion, const char* loaderBuildTag);
    // Put before the pattern list when the preflight fails on a fallback (CRLF, ends in a blank line).
    std::wstring PreflightNote(uint32_t runningCL, uint32_t newestCL);
}

#endif // MODLOADER_CLIENT_BUILD
