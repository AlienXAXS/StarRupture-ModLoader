#pragma once

// ---------------------------------------------------------------------------
// The wait for engine init, and what the loader says when it ran out
//
// The pieces of the Stage 2 wait that need no game, as plain functions of their
// inputs so they can be tested without one.
//
// The wait for the engine's init signal has a fixed deadline. On a game build
// other than the SDK's own the loader then switches plugins off, because the
// layout check could not run. When the wait ran out that is not a layout
// mismatch -- nothing was compared -- and the player is told what happened: the
// game did not finish starting. A check that ran and found a changed layout, or
// a hook that failed to install (so no wait took place), keeps its own wording.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace StartupWait
{
    // How long the init thread waits for the engine's init signal.
    constexpr uint32_t kEngineTimeoutMs = 120'000;

    // Milliseconds left until `deadlineMs`, 0 once it has passed. Both are
    // GetTickCount64-style times in milliseconds.
    inline uint32_t RemainingMs(uint64_t nowMs, uint64_t deadlineMs)
    {
        return nowMs >= deadlineMs ? 0 : static_cast<uint32_t>(deadlineMs - nowMs);
    }

    // Why the loader switched every plugin off on the layout check's account.
    enum class SafeOffKind
    {
        Mismatch,    // the layout changed, or a value the loader needs was not found
        CheckNotRun, // the game did not finish starting within the wait: nothing was compared
    };

    // CheckNotRun only when engine init was never observed and the wait is what
    // ran out. Without a wait that ran out (the engine-init hook failed to
    // install, so the wait ended at once) the game was not given the time, and
    // the plain "mismatch" wording stays.
    inline SafeOffKind ClassifySafeOff(bool engineInitObserved, bool engineWaitTimedOut)
    {
        return (!engineInitObserved && engineWaitTimedOut) ? SafeOffKind::CheckNotRun : SafeOffKind::Mismatch;
    }

    inline uint32_t TimeoutSeconds() { return kEngineTimeoutMs / 1000; }

    // The log line for the CheckNotRun case.
    inline std::wstring NotCheckedLogLine()
    {
        return L"[init] The game did not finish starting within " + std::to_wstring(TimeoutSeconds())
            + L" seconds, so the layout check could not run -- plugins are switched off for this session";
    }

    inline const wchar_t* NotCheckedHint()
    {
        return L"The game may be stuck on a message box of its own, or its files may be damaged "
               L"(verify the game files in Steam).";
    }

    // The one-sentence summary, for the dialog's fallback message box.
    inline std::wstring NotCheckedSummary()
    {
        return L"The game did not finish starting within " + std::to_wstring(TimeoutSeconds())
            + L" seconds, so the ModLoader could not check it. Its plugins are switched off for this session.";
    }

    // The dialog's header text for the CheckNotRun case (CRLF: the dialog's static
    // control does not render bare '\n' either).
    inline std::wstring NotCheckedHeader()
    {
        return std::wstring(L"The game did not finish starting within ") + std::to_wstring(TimeoutSeconds())
            + L" seconds, so the ModLoader could not check it.\r\n"
              L"\r\n"
              L"At startup the ModLoader waits for the game to finish starting, then checks that "
              L"the game data its plugins read is where they expect it. That check never ran, and "
              L"this is not the game build the ModLoader was made for, so it has switched its "
              L"plugins off for this session: none will be loaded. This is deliberate, not a bug.\r\n"
              L"\r\n"
            + NotCheckedHint() + L"\r\n"
              L"\r\n"
              L"You can play right now without plugins, or quit and try again. Your saves are "
              L"untouched either way.";
    }
}
