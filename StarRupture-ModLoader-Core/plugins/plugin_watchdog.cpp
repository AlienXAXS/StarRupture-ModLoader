#include "pch.h"
#include "plugin_watchdog.h"
#include "hooks/hooks_common.h"
#include "logging/logger.h"
#include <atomic>

namespace PluginWatchdog
{
    // A call still running after this long is reported. Five seconds is when
    // Windows itself marks the game "Not Responding".
    static constexpr ULONGLONG kStuckAfterMs = 5000;

    // How often the running calls are checked, and how late Windows may run a
    // check so it can batch it with other timers.
    static constexpr DWORD kCheckPeriodMs = 1000;
    static constexpr DWORD kCheckWindowMs = 500;

    // Threads that call into plugins at the same time: the game thread and the
    // render thread, with room to spare. A thread beyond this is not watched.
    static constexpr size_t kMaxThreads = 8;

    // One per thread that calls into plugins, written only by that thread and
    // read by the timer. The call fields form a seqlock: `seq` is odd while the
    // owner rewrites them, and the timer keeps what it read only if `seq` was
    // even and unchanged around the read.
    struct Slot
    {
        std::atomic<DWORD>       thread{ 0 };       // owner, 0 while free
        std::atomic<uint64_t>    seq{ 0 };
        std::atomic<uint64_t>    id{ 0 };
        std::atomic<const void*> code{ nullptr };
        std::atomic<const char*> what{ nullptr };
        std::atomic<ULONGLONG>   startMs{ 0 };
        std::atomic<uint64_t>    reported{ 0 };     // id of the last call reported stuck
        uint64_t                 lastId = 0;        // owner only; kept across owners so ids never repeat
    };

    static Slot      s_slots[kMaxThreads];
    static PTP_TIMER s_timer = nullptr;

    // Holds the calling thread's slot until the thread exits.
    struct SlotLease
    {
        Slot* slot = nullptr;

        SlotLease()
        {
            const DWORD self = GetCurrentThreadId();
            for (Slot& s : s_slots)
            {
                DWORD free = 0;
                if (s.thread.compare_exchange_strong(free, self))
                {
                    slot = &s;
                    break;
                }
            }
        }

        ~SlotLease()
        {
            if (slot)
                slot->thread.store(0, std::memory_order_release);
        }
    };

    static Slot* CurrentSlot()
    {
        static thread_local SlotLease lease;
        return lease.slot;
    }

    static void Write(Slot& slot, const Call& call)
    {
        const uint64_t seq = slot.seq.load(std::memory_order_relaxed);
        slot.seq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.id.store(call.id, std::memory_order_relaxed);
        slot.code.store(call.code, std::memory_order_relaxed);
        slot.what.store(call.what, std::memory_order_relaxed);
        slot.startMs.store(call.startMs, std::memory_order_relaxed);
        slot.seq.store(seq + 2, std::memory_order_release);
    }

    // False if the owner was mid-write; the next check reads it again.
    static bool Read(const Slot& slot, Call& call)
    {
        const uint64_t seq = slot.seq.load(std::memory_order_acquire);
        if (seq & 1)
            return false;
        call.id      = slot.id.load(std::memory_order_relaxed);
        call.code    = slot.code.load(std::memory_order_relaxed);
        call.what    = slot.what.load(std::memory_order_relaxed);
        call.startMs = slot.startMs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot.seq.load(std::memory_order_relaxed) == seq;
    }

    static void CALLBACK CheckCalls(PTP_CALLBACK_INSTANCE, PVOID, PTP_TIMER)
    {
        for (Slot& slot : s_slots)
        {
            Call call;
            if (!Read(slot, call) || !call.id)
                continue;

            // The clock is read after the slot, so a call that started during
            // this check cannot look older than it is.
            const ULONGLONG ranMs = GetTickCount64() - call.startMs;
            if (ranMs < kStuckAfterMs)
                continue;

            // Once per call. Two checks can overlap when one is held up
            // writing the log, so only the one that claims the call reports it.
            uint64_t last = slot.reported.load(std::memory_order_relaxed);
            if (last == call.id || !slot.reported.compare_exchange_strong(last, call.id))
                continue;

            ModLoaderLogger::LogWarn(L"[PluginWatchdog] %S %S has been running for %llu ms on thread %lu; "
                                     L"if the game is frozen, it is stuck in this plugin",
                                     Hooks::GetCallerModuleName(const_cast<void*>(call.code)).c_str(), call.what,
                                     ranMs, slot.thread.load(std::memory_order_relaxed));
        }
    }

    void Start()
    {
        s_timer = CreateThreadpoolTimer(CheckCalls, nullptr, nullptr);
        if (!s_timer)
        {
            ModLoaderLogger::LogWarn(L"[PluginWatchdog] CreateThreadpoolTimer failed (%lu); a hang in a plugin "
                                     L"will not be reported", GetLastError());
            return;
        }

        // Negative: relative to now, in 100 ns units.
        ULARGE_INTEGER due;
        due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(kCheckPeriodMs) * 10'000);
        FILETIME firstCheck{ due.LowPart, due.HighPart };
        SetThreadpoolTimer(s_timer, &firstCheck, kCheckPeriodMs, kCheckWindowMs);
    }

    void Stop()
    {
        if (!s_timer)
            return;
        SetThreadpoolTimer(s_timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(s_timer, TRUE);
        CloseThreadpoolTimer(s_timer);
        s_timer = nullptr;
    }

    Scope::Scope(const void* code, const char* what)
        : m_slot(CurrentSlot())
    {
        if (!m_slot)
            return;

        // Only this thread writes its slot, so its own reads need no seqlock.
        m_outer.id      = m_slot->id.load(std::memory_order_relaxed);
        m_outer.code    = m_slot->code.load(std::memory_order_relaxed);
        m_outer.what    = m_slot->what.load(std::memory_order_relaxed);
        m_outer.startMs = m_slot->startMs.load(std::memory_order_relaxed);

        m_call = { ++m_slot->lastId, code, what, GetTickCount64() };
        Write(*m_slot, m_call);
    }

    Scope::~Scope()
    {
        if (!m_slot)
            return;

        Write(*m_slot, m_outer);

        if (m_slot->reported.load(std::memory_order_relaxed) == m_call.id)
            ModLoaderLogger::LogWarn(L"[PluginWatchdog] %S %S returned after %llu ms",
                                     Hooks::GetCallerModuleName(const_cast<void*>(m_call.code)).c_str(), m_call.what,
                                     GetTickCount64() - m_call.startMs);
    }
}
