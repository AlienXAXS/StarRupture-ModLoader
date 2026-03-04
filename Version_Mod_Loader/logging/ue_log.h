#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <atomic>
#include <string>

// ---------------------------------------------------------------------------
// UELog — forwards mod loader messages into the game's own log (StarRupture.log)
//
// Uses UE::Logging::Private::BasicLogV, the internal function that all
// UE_LOG macros funnel through in UE 5.3+ shipping builds.
//
// Signature (x64 __fastcall):
//   void BasicLogV(const FLogCategoryBase* Category,
//                  const FStaticBasicLogRecord* Record,
//                  char* Args)           <- pointer to first variadic arg on stack
//
// We construct a minimal FLogCategoryBase (just enough fields that BasicLogV
// reads) and a FStaticBasicLogRecord with a pre-formatted message so we never
// pass variadic args at all — we embed the final string directly in the format
// field and pass nullptr for Args.
//
// Thread safety: Initialize() uses an atomic flag; Write() is safe to call
// from any thread once initialized, because BasicLogV uses its own internal
// locks.
// ---------------------------------------------------------------------------

namespace UELog
{
    // -----------------------------------------------------------------------
    // Minimal UE type layouts — only the fields BasicLogV actually reads.
    // Verified against UE 5.3 source (Engine/Source/Runtime/Core/Public/Logging/).
    // -----------------------------------------------------------------------

    // ELogVerbosity::Type values that BasicLogV understands
    enum class ELogVerbosity : uint8_t
    {
        NoLogging   = 0,
        Fatal       = 1,
        Error       = 2,
        Warning     = 3,
        Display     = 4,
        Log         = 5,
        Verbose     = 6,
        VeryVerbose = 7,
    };

    // FLogCategoryBase — layout verified from IDA analysis of the game binary:
    //   +0x00  Verbosity             (ELogVerbosity::Type / uint8)
    //   +0x01  DebugBreakOnLog       (bool)
    //   +0x02  DefaultVerbosity      (uint8)
    //   +0x03  CompileTimeVerbosity  (const uint8) — compile-time ceiling
    //   +0x04  CategoryName          (FName, 8 bytes: ComparisonIndex + Number)
    //   sizeof = 0x0C
    #pragma pack(push, 1)
    struct FLogCategoryBase
    {
        uint8_t  Verbosity;             // 0x00 — runtime verbosity
        bool     DebugBreakOnLog;       // 0x01
        uint8_t  DefaultVerbosity;      // 0x02
        uint8_t  CompileTimeVerbosity;  // 0x03 — compile-time ceiling
        uint32_t NameComparisonIndex;   // 0x04 — FName comparison index
        uint32_t NameNumber;            // 0x08 — FName number (0 = no suffix)
    };
    #pragma pack(pop)
    static_assert(sizeof(FLogCategoryBase) == 0x0C, "FLogCategoryBase size mismatch");

    // FStaticBasicLogRecord — BasicLogV reads:
    //   +0x00  Format   (const wchar_t*)  — format string (we embed final msg here)
    //   +0x08  File     (const char*)     — source file (for log metadata)
    //   +0x10  Line     (int32)           — source line
    //   +0x14  Verbosity (uint8)          — verbosity of this specific log call
    //   +0x15  _pad[3]
    #pragma pack(push, 1)
    struct FStaticBasicLogRecord
    {
        const wchar_t* Format;          // 0x00
        const char*    File;            // 0x08
        int32_t        Line;            // 0x10
        uint8_t        Verbosity;       // 0x14
        uint8_t        _pad[3];         // 0x15
    };
    #pragma pack(pop)
    static_assert(sizeof(FStaticBasicLogRecord) == 0x18, "FStaticBasicLogRecord size mismatch");

    // -----------------------------------------------------------------------
    // BasicLogV function pointer type
    // -----------------------------------------------------------------------
    typedef void(__fastcall* BasicLogV_t)(
        const FLogCategoryBase*      Category,
        const FStaticBasicLogRecord* Record,
        char*                        Args);

    // -----------------------------------------------------------------------
    // Module-level state — all initialised to zero/false/nullptr
    // -----------------------------------------------------------------------
    inline std::atomic<bool>  g_initialized{ false };
    inline BasicLogV_t        g_basicLogV  = nullptr;

    // Persistent category — constructed once in Initialize(), lives forever.
    // We never tear this down because the engine may call into it after our
    // DLL would normally clean up.
    inline FLogCategoryBase   g_category{};

    // Pattern for UE::Logging::Private::BasicLogV (provided by user via IDA)
    inline constexpr const char* BASIC_LOGV_PATTERN =
        "4C 8B DC 55 57 41 57 49 8D 6B ?? 48 81 EC ?? ?? ?? ?? 80 3D";

    // -----------------------------------------------------------------------
    // Initialize — call once from the EngineInit callback.
    // Scans for BasicLogV and builds the persistent log category.
    // findPattern: function with signature uintptr_t(const std::string&),
    //              e.g. Scanner::FindPatternInMainModule.
    // Returns true if the UE log bridge is ready.
    // -----------------------------------------------------------------------
    inline bool Initialize(uintptr_t (*findPattern)(const std::string&))
    {
        // Idempotent — safe to call more than once
        if (g_initialized.load(std::memory_order_acquire))
            return true;

        if (!findPattern)
            return false;

        uintptr_t addr = findPattern(std::string(BASIC_LOGV_PATTERN));
        if (!addr)
            return false;

        g_basicLogV = reinterpret_cast<BasicLogV_t>(addr);

        // Build the FLogCategoryBase.
        // Set verbosity = Log (5), compile-time ceiling = VeryVerbose (7) so
        // BasicLogV's internal suppression check never filters us out.
        // DebugBreakOnLog = false so we never trigger a debug break.
        // CategoryName FName = {0,0} (index 0, no number suffix).
        g_category.Verbosity            = static_cast<uint8_t>(ELogVerbosity::Log);
        g_category.DebugBreakOnLog      = false;
        g_category.DefaultVerbosity     = static_cast<uint8_t>(ELogVerbosity::Log);
        g_category.CompileTimeVerbosity = static_cast<uint8_t>(ELogVerbosity::VeryVerbose);
        g_category.NameComparisonIndex  = 0;
        g_category.NameNumber           = 0;

        g_initialized.store(true, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Write — forward a pre-formatted narrow string to the UE log.
    // verbosity: use ELogVerbosity values above.
    // message:   plain narrow (UTF-8/ASCII) string — we convert to wide here.
    // -----------------------------------------------------------------------
    inline void Write(ELogVerbosity verbosity, const char* message)
    {
        if (!g_initialized.load(std::memory_order_acquire))
            return;

        if (!g_basicLogV || !message)
            return;

        // Convert to wide.  The record holds a pointer to this buffer, so it
        // must stay alive across the BasicLogV call.  Stack-allocated is fine.
        wchar_t wbuf[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, message, -1, wbuf, 2048) == 0)
            return;

        // Build record on the stack — BasicLogV does not retain it
        FStaticBasicLogRecord record{};
        record.Format    = wbuf;
        record.File      = __FILE__;
        record.Line      = 0;
        record.Verbosity = static_cast<uint8_t>(verbosity);

        // Args = nullptr because our format string IS the final message
        // (no % specifiers), so BasicLogV won't try to va_arg anything.
        __try
        {
            g_basicLogV(&g_category, &record, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // If the engine log system is torn down before we stop forwarding,
            // silently swallow the exception rather than crashing.
        }
    }

    // Convenience wrappers matching our Log:: level names
    inline void Info   (const char* msg) { Write(ELogVerbosity::Log,     msg); }
    inline void Warning(const char* msg) { Write(ELogVerbosity::Warning,  msg); }
    inline void Error  (const char* msg) { Write(ELogVerbosity::Error,    msg); }

} // namespace UELog
