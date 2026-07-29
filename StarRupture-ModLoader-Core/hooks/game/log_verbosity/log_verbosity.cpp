#include "pch.h"
#include "log_verbosity.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "core/startup_utils.h"
#include "../scan_patterns.h"
#include <cstring>
#include <cwchar>
#include <string>

namespace Hooks::LogVerbosity
{
    // -----------------------------------------------------------------------
    // Resolved engine symbols
    // -----------------------------------------------------------------------

    // void __fastcall FLogSuppressionImplementation::ApplyGlobalChanges(void* this)
    using ApplyGlobalChanges_t = void(__fastcall*)(void* thisPtr);
    static ApplyGlobalChanges_t g_applyGlobalChanges = nullptr;

    // void* __fastcall FLogSuppressionInterface::Get()
    using LogSuppressionGet_t = void*(__fastcall*)();
    static LogSuppressionGet_t g_logSuppressionGet = nullptr;

    // The Verbosity byte of the static "Global" FLogCategoryBase. Verbosity is
    // at offset 0 of FLogCategoryBase in this build (ApplyGlobalChanges reads
    // *cat for verbosity, cat[1] for break-on-log, cat[3] for the compile-time
    // cap), so this points directly at the byte we write.
    static volatile unsigned char* g_globalVerbosityByte = nullptr;

    // Cached from the ProcessConfigAndCommandLine detour -- this is the
    // FLogSuppressionImplementation singleton and is what ApplyGlobalChanges
    // expects as its `this`.
    static void* g_suppressionSingleton = nullptr;

    // void __fastcall FLogSuppressionImplementation::ProcessConfigAndCommandLine(void* this)
    using ProcessConfigAndCommandLine_t = void(__fastcall*)(void* thisPtr);
    static Hook                         g_processConfigHook;
    static ProcessConfigAndCommandLine_t g_processConfigOriginal = nullptr;

    static bool  g_installed = false;
    static Level g_level     = Level::Default;

    // -----------------------------------------------------------------------
    // Level <-> ini name
    // -----------------------------------------------------------------------
    const wchar_t* LevelToIniName(Level level)
    {
        switch (level)
        {
        case Level::Error:       return L"Error";
        case Level::Warning:     return L"Warning";
        case Level::Display:     return L"Display";
        case Level::Log:         return L"Log";
        case Level::Verbose:     return L"Verbose";
        case Level::VeryVerbose: return L"VeryVerbose";
        default:                 return L"Default";
        }
    }

    Level LevelFromIniName(const wchar_t* name)
    {
        if (!name) return Level::Default;
        if (_wcsicmp(name, L"Error")       == 0) return Level::Error;
        if (_wcsicmp(name, L"Warning")     == 0) return Level::Warning;
        if (_wcsicmp(name, L"Display")     == 0) return Level::Display;
        if (_wcsicmp(name, L"Log")         == 0) return Level::Log;
        if (_wcsicmp(name, L"Verbose")     == 0) return Level::Verbose;
        if (_wcsicmp(name, L"VeryVerbose") == 0) return Level::VeryVerbose;
        return Level::Default;
    }

    Level GetLevel() { return g_level; }

    // -----------------------------------------------------------------------
    // Apply
    // -----------------------------------------------------------------------

    // Native call into engine internals -- kept in its own SEH-guarded helper
    // with no C++ objects in scope (C2712).
    static bool PushGlobalVerbosity(void* singleton, unsigned char raw)
    {
        __try
        {
            *g_globalVerbosityByte = raw;
            g_applyGlobalChanges(singleton);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void* ResolveSingleton()
    {
        if (g_suppressionSingleton)
            return g_suppressionSingleton;

        if (!g_logSuppressionGet)
            return nullptr;

        // Get() lazily constructs the singleton; safe to call at any point
        // after CRT static init.
        __try
        {
            g_suppressionSingleton = g_logSuppressionGet();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_suppressionSingleton = nullptr;
        }
        return g_suppressionSingleton;
    }

    bool Apply(Level level)
    {
        if (level == Level::Default)
            return true;   // explicit no-op; nothing to restore

        if (!g_applyGlobalChanges || !g_globalVerbosityByte)
        {
            ModLoaderLogger::LogWarn(
                L"[LogVerbosity] Cannot apply -- engine symbols were not resolved");
            return false;
        }

        void* singleton = ResolveSingleton();
        if (!singleton)
        {
            ModLoaderLogger::LogWarn(
                L"[LogVerbosity] Cannot apply -- FLogSuppression singleton unavailable");
            return false;
        }

        // ApplyGlobalChanges works on the delta against the verbosity it last
        // saw, so setting the byte to a value it already holds does nothing.
        // Drop to Fatal first to establish a known-low baseline, then raise to
        // the requested level. Each category ends at
        // min(level, its CompileTimeVerbosity).
        if (!PushGlobalVerbosity(singleton, 1 /* ELogVerbosity::Fatal */))
        {
            ModLoaderLogger::LogError(L"[LogVerbosity] Exception while lowering global verbosity");
            return false;
        }

        const unsigned char raw = static_cast<unsigned char>(static_cast<int>(level));
        if (!PushGlobalVerbosity(singleton, raw))
        {
            ModLoaderLogger::LogError(L"[LogVerbosity] Exception while raising global verbosity");
            return false;
        }

        ModLoaderLogger::LogInfo(L"[LogVerbosity] Game log verbosity set to %s (%d)",
                                 LevelToIniName(level), static_cast<int>(level));
        return true;
    }

    void SetLevel(Level level)
    {
        g_level = level;

        const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");
        WritePrivateProfileStringW(L"Logging", L"GameVerbosity",
                                   LevelToIniName(level), iniPath.c_str());

        Apply(level);
    }

    // -----------------------------------------------------------------------
    // Detour
    // -----------------------------------------------------------------------
    static void __fastcall ProcessConfigAndCommandLine_Detour(void* thisPtr)
    {
        // Cache the singleton -- this is the only place we are handed it for
        // free, and the settings UI needs it later for live changes.
        g_suppressionSingleton = thisPtr;

        if (g_processConfigOriginal)
            g_processConfigOriginal(thisPtr);

        // Re-apply on return: the original has just stamped every category from
        // [Core.Log] and -LogCmds, discarding anything set before now.
        if (g_level != Level::Default)
        {
            ModLoaderLogger::LogInfo(
                L"[LogVerbosity] ProcessConfigAndCommandLine returned -- applying override");
            Apply(g_level);
        }
    }

    // -----------------------------------------------------------------------
    // Install
    // -----------------------------------------------------------------------

    // ApplyGlobalChanges loads the Global category's verbosity byte with
    //   44 0F B6 15 <rel32>     movzx r10d, byte ptr [rip+rel32]
    // as its first movzx. Decode that to get the byte's absolute address rather
    // than needing a separate data signature.
    static volatile unsigned char* ResolveGlobalVerbosityByte(uintptr_t applyGlobalChanges)
    {
        constexpr size_t kMaxScan = 0x80;
        const auto* p = reinterpret_cast<const unsigned char*>(applyGlobalChanges);

        for (size_t i = 0; i < kMaxScan; ++i)
        {
            if (p[i] == 0x44 && p[i + 1] == 0x0F && p[i + 2] == 0xB6 && p[i + 3] == 0x15)
            {
                int32_t rel = 0;
                memcpy(&rel, p + i + 4, sizeof(rel));
                const uintptr_t nextInsn = applyGlobalChanges + i + 8;
                return reinterpret_cast<volatile unsigned char*>(nextInsn + rel);
            }
        }
        return nullptr;
    }

    bool Install()
    {
        if (g_installed)
            return true;

        ModLoaderLogger::LogInfo(L"[LogVerbosity] Installing game log verbosity control...");

        const HMODULE   mainModule = GetModuleHandleW(nullptr);
        const uintptr_t base       = reinterpret_cast<uintptr_t>(mainModule);

        // Persisted level first -- if the user never enabled this we still want
        // the detour installed so a live change from the UI works this session.
        const std::wstring iniPath = GetModLoaderDirPath(L"modloader.ini");
        wchar_t levelBuf[32] = {};
        GetPrivateProfileStringW(L"Logging", L"GameVerbosity", L"Default",
                                 levelBuf, 32, iniPath.c_str());
        g_level = LevelFromIniName(levelBuf);
        ModLoaderLogger::LogDebug(L"[LogVerbosity] Persisted level: %s", LevelToIniName(g_level));

        // ApplyGlobalChanges -- the push mechanism, and the source of the
        // Global category's verbosity byte.
        const uintptr_t applyAddr = Scanner::FindPatternInMainModule(
            "FLogSuppressionImplementation::ApplyGlobalChanges",
            ScanPatterns::FLogSuppression_ApplyGlobalChanges);

        if (!applyAddr)
        {
            ModLoaderLogger::LogWarn(
                L"[LogVerbosity] [FAIL] ApplyGlobalChanges pattern not found -- feature disabled");
            return false;
        }

        g_applyGlobalChanges = reinterpret_cast<ApplyGlobalChanges_t>(applyAddr);
        ModLoaderLogger::LogDebug(L"[LogVerbosity] [OK] ApplyGlobalChanges at 0x%llX (base+0x%llX)",
                                  static_cast<unsigned long long>(applyAddr),
                                  static_cast<unsigned long long>(applyAddr - base));

        g_globalVerbosityByte = ResolveGlobalVerbosityByte(applyAddr);
        if (!g_globalVerbosityByte)
        {
            ModLoaderLogger::LogWarn(
                L"[LogVerbosity] [FAIL] Could not decode the Global verbosity byte reference "
                L"-- feature disabled");
            g_applyGlobalChanges = nullptr;
            return false;
        }
        ModLoaderLogger::LogDebug(L"[LogVerbosity] [OK] Global verbosity byte at 0x%llX (base+0x%llX)",
                                  static_cast<unsigned long long>(
                                      reinterpret_cast<uintptr_t>(g_globalVerbosityByte)),
                                  static_cast<unsigned long long>(
                                      reinterpret_cast<uintptr_t>(g_globalVerbosityByte) - base));

        // FLogSuppressionInterface::Get -- fallback source for the singleton if
        // we attached after ProcessConfigAndCommandLine had already run (the
        // QueueUserAPC path in core_entry.cpp can land that late).
        const uintptr_t getAddr = Scanner::FindPatternInMainModule(
            "FLogSuppressionInterface::Get",
            ScanPatterns::FLogSuppression_Get);

        if (getAddr)
        {
            g_logSuppressionGet = reinterpret_cast<LogSuppressionGet_t>(getAddr);
            ModLoaderLogger::LogDebug(L"[LogVerbosity] [OK] FLogSuppressionInterface::Get at 0x%llX",
                                      static_cast<unsigned long long>(getAddr));
        }
        else
        {
            ModLoaderLogger::LogWarn(
                L"[LogVerbosity] FLogSuppressionInterface::Get not found -- live changes will only "
                L"work once ProcessConfigAndCommandLine has run");
        }

        // ProcessConfigAndCommandLine -- the thing that would overwrite us.
        const uintptr_t processAddr = Scanner::FindPatternInMainModule(
            "FLogSuppressionImplementation::ProcessConfigAndCommandLine",
            ScanPatterns::FLogSuppression_ProcessConfigAndCommandLine);

        if (processAddr)
        {
            const bool hookOk = g_processConfigHook.Install(
                processAddr,
                reinterpret_cast<void*>(&ProcessConfigAndCommandLine_Detour),
                reinterpret_cast<void**>(&g_processConfigOriginal));

            if (hookOk)
            {
                ModLoaderLogger::LogInfo(
                    L"[LogVerbosity] [OK] ProcessConfigAndCommandLine hook installed at 0x%llX (base+0x%llX)",
                    static_cast<unsigned long long>(processAddr),
                    static_cast<unsigned long long>(processAddr - base));
            }
            else
            {
                ModLoaderLogger::LogWarn(
                    L"[LogVerbosity] [FAIL] ProcessConfigAndCommandLine hook failed -- applying now instead");
            }
        }
        else
        {
            ModLoaderLogger::LogWarn(
                L"[LogVerbosity] [FAIL] ProcessConfigAndCommandLine pattern not found "
                L"-- applying now instead");
        }

        g_installed = true;

        // If the detour is not in place we may already be past PreInit, so apply
        // immediately. Harmless if it is in place and PreInit has not run: the
        // singleton simply is not available yet and the detour does it later.
        if (!g_processConfigOriginal && g_level != Level::Default)
            Apply(g_level);

        return true;
    }

    void Remove()
    {
        if (!g_installed)
            return;

        g_processConfigHook.Remove();
        g_processConfigOriginal = nullptr;
        g_installed             = false;
    }

    bool IsInstalled() { return g_installed; }
}

#endif // MODLOADER_CLIENT_BUILD
