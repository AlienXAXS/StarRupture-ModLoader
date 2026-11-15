#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>

// ---------------------------------------------------------------------------
// EngineCall: small helpers for code that calls into the engine
// ---------------------------------------------------------------------------

namespace EngineCall
{
    // A bad pointer inside the engine reads as an access violation or a page
    // error. Anything else (a breakpoint, a stack overflow) is not ours to swallow.
    inline int SehFilter(DWORD code)
    {
        return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
    }

    // The same filter, and the exception code is kept for the log line.
    inline int SehFilter(DWORD code, DWORD* out)
    {
        *out = code;
        return SehFilter(code);
    }

    // UTF-8 to UTF-16, for the engine's wide-character paths.
    inline std::wstring Widen(const std::string& text)
    {
        if (text.empty())
            return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    }
}
