#pragma once

// ============================================================
// AdminPrompt -- the client's side of the admin prompt ($ML:adminq in,
// $ML:adminr out), without any drawing.
//
// A server with no admin asks a joining player to claim it with a code from
// its log. This file keeps what the player's window needs to show and sends the
// two answers. The window itself reads GetSnapshot() every frame and calls
// Submit() / NotMe() from its buttons.
//
// Client builds only (admin_prompt.cpp is left out of the other configurations);
// the state steps below are plain functions with no engine access.
// ============================================================

#include "host_rules_wire.h"

#include <cstdint>
#include <string>

namespace AdminPrompt
{
    // What the window shows above the text box while the server is asking.
    constexpr const char* kPromptText =
        "This server has no admin yet. If you run this server, enter the claim code from the "
        "server log. Otherwise choose Not me.";

    // What a window needs to draw. open: show the window. final: the server has
    // ended the prompt, so there is no text box, only status (the last line) and
    // a button that calls NotMe() to dismiss it. status: the last line the
    // server sent ("Wrong code", "You are the admin"), empty until it sends one.
    struct Snapshot
    {
        bool        open  = false;
        bool        final = false;
        std::string status;
    };

    struct State
    {
        bool        open  = false;
        bool        final = false;
        std::string status;
        bool        hasLastAnswer = false; // an answer went out, at lastAnswerMs
        uint64_t    lastAnswerMs  = 0;
        bool        hasDismiss = false; // the player dismissed the prompt, at dismissMs
        uint64_t    dismissMs  = 0;
    };

    // After the player dismisses the prompt, a new ask on the same connection is
    // ignored for this long, so a server (or a peer spoofing one) cannot pull the
    // window back up again and again.
    constexpr uint64_t kAskLatchMs = 60000;

    // Whether an ask arriving at nowMs falls inside the latch. A clock that went
    // back counts as inside.
    inline bool AskLatched(bool hasDismiss, uint64_t nowMs, uint64_t dismissMs)
    {
        return hasDismiss && (nowMs < dismissMs || nowMs - dismissMs < kAskLatchMs);
    }

    // $ML:adminq arrived. ask opens (or reopens) the prompt; info replaces the
    // status line and keeps it open; close sets the last line and ends the
    // input. A line for a prompt that is not showing (never asked, or the player
    // already dismissed it) is dropped, and so is an ask inside the latch of
    // AskLatched. The info and close lines of a prompt that is showing always apply.
    inline void ApplyQuestion(State& s, HostRulesWire::AdminQKind kind, const std::string& text, uint64_t nowMs)
    {
        switch (kind)
        {
        case HostRulesWire::AdminQKind::Ask:
            if (AskLatched(s.hasDismiss, nowMs, s.dismissMs)) break;
            s.open = true;
            s.final = false;
            s.status.clear();
            s.hasLastAnswer = false;
            break;
        case HostRulesWire::AdminQKind::Info:
            if (s.open && !s.final) s.status = text;
            break;
        case HostRulesWire::AdminQKind::Close:
            if (s.open && !s.final)
            {
                s.status = text;
                s.final = true;
            }
            break;
        }
    }

    // Whether "Not me" should put an answer on the wire: only for a prompt still
    // asking, and no sooner than 500 ms after the last answer (the server drops
    // an earlier one). The window closes either way.
    inline bool NotMeSendsAnswer(const State& s, uint64_t nowMs)
    {
        return s.open && !s.final &&
               HostRulesWire::DecideAdminAnswer(true, nowMs, s.hasLastAnswer, s.lastAnswerMs) ==
                   HostRulesWire::AdminAnswerDecision::Accept;
    }

    // An answer went out at nowMs.
    inline void NoteAnswerSent(State& s, uint64_t nowMs)
    {
        s.hasLastAnswer = true;
        s.lastAnswerMs = nowMs;
    }

    // The player is done with the window ("Not me", the title bar's X, or
    // dismissing a last line).
    inline void CloseLocally(State& s, uint64_t nowMs)
    {
        s.hasDismiss = true;
        s.dismissMs = nowMs;
        s.open = false;
        s.final = false;
        s.status.clear();
    }

    // Leaving the server: nothing of this prompt belongs to the next one.
    inline void Reset(State& s)
    {
        s = State{};
    }

    enum class SubmitResult
    {
        Ok,          // sent
        NotOpen,     // no prompt waiting for a code
        Empty,       // nothing typed, or only spaces
        TooLong,     // over the 64-byte code limit
        ControlChar, // a control character in the code
        RateLimited, // an answer went out less than 500 ms ago
        SendFailed,  // no proven connection to send on
    };

    // Whether a typed code may go out now. Leading and trailing spaces are
    // dropped (a pasted code often has one); on Ok the code to send is in *out.
    inline SubmitResult CheckSubmit(const State& s, const char* code, uint64_t nowMs, std::string* out)
    {
        if (!s.open || s.final) return SubmitResult::NotOpen;

        std::string c = code ? code : "";
        const size_t b = c.find_first_not_of(' ');
        if (b == std::string::npos) return SubmitResult::Empty;
        c = c.substr(b, c.find_last_not_of(' ') - b + 1);

        if (c.size() > HostRulesWire::kMaxAdminRCode) return SubmitResult::TooLong;
        for (unsigned char ch : c)
            if (ch < 0x20 || ch == 0x7F) return SubmitResult::ControlChar;

        if (HostRulesWire::DecideAdminAnswer(true, nowMs, s.hasLastAnswer, s.lastAnswerMs) !=
            HostRulesWire::AdminAnswerDecision::Accept)
            return SubmitResult::RateLimited;

        *out = std::move(c);
        return SubmitResult::Ok;
    }

    // ---- The live state (admin_prompt.cpp). Any thread; one mutex inside. ----

    // A copy of what to draw now.
    Snapshot GetSnapshot();

    // The Send button: validates the typed code (NUL-terminated, as the text
    // box holds it) and sends it. The prompt stays open until the server answers
    // with info or close. The code is neither logged nor kept.
    SubmitResult Submit(const char* code);

    // The Not me button, and the dismiss button of a final line: sends "not me"
    // while the prompt is still asking, and closes the window.
    void NotMe();

    // A decoded $ML:adminq from the server (network_channel.cpp).
    void OnQuestion(const HostRulesWire::AdminQuestion& q);

    // Leaving the server: close and clear everything.
    void Leave();
}
