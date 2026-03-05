#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <tlhelp32.h>
#include <limits>

// ---------------------------------------------------------------------------
// Returns the thread ID of the oldest thread in the current process,
// which is conventionally the process main thread.
//
// NOTE: The snapshot enumeration skips the very first entry returned by
// Thread32First (it is only used to seed the iterator); all subsequent
// entries are examined via Thread32Next.  This matches the UE4SS reference
// implementation.  If the process has only one thread, this function returns
// 0 (no Thread32Next entries exist to compare).
// ---------------------------------------------------------------------------
inline auto get_main_thread_id() -> DWORD
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    DWORD currentPid = GetCurrentProcessId();
    THREADENTRY32 th32;
    th32.dwSize = sizeof(THREADENTRY32);

    uint64_t earliestCreationTime = (std::numeric_limits<uint64_t>::max)();
    DWORD mainThreadId = 0;

    for (Thread32First(snapshot, &th32); Thread32Next(snapshot, &th32);)
    {
        if (th32.th32OwnerProcessID != currentPid)
        {
            continue;
        }

        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, false, th32.th32ThreadID);
        if (!thread)
        {
            continue;
        }

        FILETIME threadTimes[4];
        if (!GetThreadTimes(thread, &threadTimes[0], &threadTimes[1], &threadTimes[2], &threadTimes[3]))
        {
            CloseHandle(thread);
            continue;
        }

        uint64_t creationTime = (static_cast<uint64_t>(threadTimes[0].dwHighDateTime) << 32)
                              | threadTimes[0].dwLowDateTime;

        if (creationTime < earliestCreationTime)
        {
            earliestCreationTime = creationTime;
            mainThreadId = th32.th32ThreadID;
        }

        CloseHandle(thread);
    }

    CloseHandle(snapshot);
    return mainThreadId;
}
