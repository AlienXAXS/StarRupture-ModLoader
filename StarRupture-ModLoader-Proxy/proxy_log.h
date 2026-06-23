#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstdarg>

// ---------------------------------------------------------------------------
// ProxyLog -- minimal OutputDebugStringA-only logging for the dwmapi.dll
// proxy shim.
//
// The proxy intentionally does NOT use LogToFile (logging/log.h, now part of
// StarRupture-ModLoader-Core.dll): LogToFile keeps its state in `inline`
// globals, which get a *separate* copy per DLL. If both the proxy and Core
// called LogToFile::Initialize() they would each open modloader.log with
// CREATE_ALWAYS, and whichever ran second would silently truncate the
// other's output. Core owns the on-disk log; the proxy only has a handful of
// early bootstrap messages (real dwmapi.dll load, Core DLL load) and a
// debugger/DebugView is sufficient for those.
// ---------------------------------------------------------------------------

namespace ProxyLog
{
    inline void Write(const char* level, const char* fmt, va_list args)
    {
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, args);

        char line[1100];
        snprintf(line, sizeof(line), "[dwmapi-proxy] [%s] %s\n", level, msg);
        OutputDebugStringA(line);
    }

    inline void Info(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        Write("INFO", fmt, args);
        va_end(args);
    }

    inline void Warn(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        Write("WARN", fmt, args);
        va_end(args);
    }

    inline void Error(const char* fmt, ...)
    {
        va_list args; va_start(args, fmt);
        Write("ERROR", fmt, args);
        va_end(args);
    }
}
