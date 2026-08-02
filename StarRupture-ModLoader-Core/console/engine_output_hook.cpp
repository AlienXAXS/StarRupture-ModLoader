#include "engine_output_hook.h"
#include "console_screen.h"

#include "hooks/hooks_common.h"
#include "logging/logger.h"

namespace EngineOutputHook
{
    using WriteConsoleW_t = BOOL(WINAPI*)(HANDLE, const void*, DWORD, DWORD*, void*);

    static Hooks::Hook     s_hook;
    static WriteConsoleW_t s_original  = nullptr;
    static bool            s_installed = false;

    // Per-thread guard. The rendering path deliberately uses RawWriteW (the
    // trampoline) so it cannot come back through here, but a console write from
    // inside anything the renderer calls would be an unbounded recursion in the
    // process's logging path -- cheap enough to make impossible rather than
    // argue about.
    static thread_local bool t_inDetour = false;

    BOOL RawWriteW(HANDLE out, const wchar_t* text, DWORD length, DWORD* written)
    {
        if (s_original)
            return s_original(out, text, length, written, nullptr);
        return WriteConsoleW(out, text, length, written, nullptr);
    }

    static BOOL WINAPI WriteConsoleW_Detour(HANDLE hConsoleOutput,
                                            const void* lpBuffer,
                                            DWORD nNumberOfCharsToWrite,
                                            DWORD* lpNumberOfCharsWritten,
                                            void* lpReserved)
    {
        if (t_inDetour || !ConsoleScreen::IsActive() || !lpBuffer || nNumberOfCharsToWrite == 0)
            return s_original(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
                              lpNumberOfCharsWritten, lpReserved);

        t_inDetour = true;

        // Consumed rather than forwarded: ConsoleScreen re-emits the text
        // itself, around the prompt and with colour applied. Forwarding as well
        // would print everything twice.
        ConsoleScreen::PrintEngineText(static_cast<const wchar_t*>(lpBuffer),
                                       static_cast<size_t>(nNumberOfCharsToWrite));

        t_inDetour = false;

        // The caller is an output device that checks nothing beyond this, but
        // report a complete write so nothing decides to retry.
        if (lpNumberOfCharsWritten)
            *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
        return TRUE;
    }

    bool Install()
    {
        if (s_installed)
            return true;

        // kernel32's export forwards to KernelBase on current Windows;
        // GetProcAddress resolves that for us, and patching the implementation
        // catches every caller whatever they imported.
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32)
        {
            ModLoaderLogger::LogError(L"[Console] kernel32.dll handle unavailable -- engine output not intercepted");
            return false;
        }

        void* target = reinterpret_cast<void*>(GetProcAddress(kernel32, "WriteConsoleW"));
        if (!target)
        {
            ModLoaderLogger::LogError(L"[Console] WriteConsoleW not found -- engine output not intercepted");
            return false;
        }

        if (!s_hook.Install(reinterpret_cast<uintptr_t>(target),
                            reinterpret_cast<void*>(&WriteConsoleW_Detour),
                            reinterpret_cast<void**>(&s_original)))
        {
            ModLoaderLogger::LogError(L"[Console] Failed to hook WriteConsoleW -- engine output not intercepted");
            return false;
        }

        s_installed = true;
        ModLoaderLogger::LogInfo(L"[Console] WriteConsoleW hooked at 0x%llX -- engine output routed through the console renderer",
                                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)));
        return true;
    }

    void Remove()
    {
        if (!s_installed)
            return;

        // Unhooking a hot Win32 API is never free: Hooks::Hook::Remove restores
        // the bytes *and* frees the trampoline, so any thread already inside
        // the detour is about to call into memory that is going away. Every
        // logging thread in the process is a candidate.
        //
        // The window cannot be closed without a real synchronisation mechanism
        // the hook API does not have, so narrow it instead: ConsoleScreen is
        // already inactive by the time this runs, which makes the detour a bare
        // forward, and this pause lets anything mid-call get out of it. The
        // alternative -- leaving the detour installed forever -- is worse: it
        // points into this DLL, which may be unloaded while the process lives.
        Sleep(50);

        s_hook.Remove();
        s_installed = false;

        // The trampoline is gone with it, so send RawWriteW back to the real
        // API rather than at freed memory.
        s_original = nullptr;

        ModLoaderLogger::LogInfo(L"[Console] WriteConsoleW hook removed");
    }

    bool IsInstalled() { return s_installed; }
}
