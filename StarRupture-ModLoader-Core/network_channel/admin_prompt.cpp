#include "pch.h"
#include "admin_prompt.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "network_channel.h"
#include "logging/logger.h"

#include <mutex>

namespace AdminPrompt
{
    // The game thread writes (a question arrives, the server is left) while the
    // window reads and answers from the render thread.
    static std::mutex g_mutex;
    static State      g_state;

    Snapshot GetSnapshot()
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        return Snapshot{ g_state.open, g_state.final, g_state.status };
    }

    SubmitResult Submit(const char* code)
    {
        const uint64_t now = GetTickCount64();
        std::string    c;

        std::lock_guard<std::mutex> lk(g_mutex);
        const SubmitResult r = CheckSubmit(g_state, code, now, &c);
        if (r != SubmitResult::Ok)
            return r;

        // The send only queues the answer, so the lock is held for no wait. The
        // time is taken only for an answer that was queued: a failed send may be
        // tried again at once.
        const bool queued = NetworkChannel::SendAdminAnswer(/*notMe*/ false, c);
        SecureZeroMemory(c.data(), c.size());
        if (!queued)
            return SubmitResult::SendFailed;

        NoteAnswerSent(g_state, now);
        return SubmitResult::Ok;
    }

    void NotMe()
    {
        const uint64_t now = GetTickCount64();

        std::lock_guard<std::mutex> lk(g_mutex);
        if (NotMeSendsAnswer(g_state, now) && NetworkChannel::SendAdminAnswer(/*notMe*/ true, std::string()))
            NoteAnswerSent(g_state, now);
        CloseLocally(g_state, now);
    }

    void OnQuestion(const HostRulesWire::AdminQuestion& q)
    {
        const uint64_t now = GetTickCount64();

        std::lock_guard<std::mutex> lk(g_mutex);
        ApplyQuestion(g_state, q.kind, q.text, now);
        ModLoaderLogger::LogDebug(L"[AdminPrompt] Server message: prompt is %s%s.",
            g_state.open ? L"open" : L"closed", g_state.final ? L" (final line)" : L"");
    }

    void Leave()
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        Reset(g_state);
    }
}

#endif // MODLOADER_CLIENT_BUILD
