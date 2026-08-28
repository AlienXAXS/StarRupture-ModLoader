#include "pch.h"
#include "net_timeout.h"
#include "logging/logger.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

// SDK headers -- paths resolved via $(StarRuptureSDKConfigDir) in Shared.props.
#include "Engine_classes.hpp"   // UWorld, UNetDriver

#include "core/startup_utils.h"

#include <string>
#include <cstdio>
#include <cwchar>

namespace NetTimeout
{
    // Defaults chosen against the failure this exists for: an 80 s map load with
    // no ticks. 300 s covers that with room to spare and is still short enough
    // that a genuinely dead peer does not hold a slot indefinitely -- which is
    // the cost of raising it, and the reason this is not simply set to an hour.
    static constexpr float kDefaultConnectionTimeout        = 300.0f;
    static constexpr float kDefaultInitialConnectTimeout    = 300.0f;

    // Refuse absurd values rather than write them: this field is read every tick
    // by the engine, and a negative or NaN timeout means every connection dies
    // instantly with no obvious cause.
    static constexpr float kMinTimeout = 1.0f;
    static constexpr float kMaxTimeout = 3600.0f;

    static float g_connectionTimeout        = 0.0f;
    static float g_initialConnectTimeout    = 0.0f;

    // Warn once per problem rather than once per tick.
    static bool  g_warnedNoDriver           = false;
    // Which driver we last logged an apply for. Cleared to null when the driver
    // goes away, so travel produces one line per driver instead of silence.
    static void* g_lastAppliedDriver        = nullptr;

    static std::wstring IniPath()
    {
        return GetModLoaderDirPath(L"modloader.ini");
    }

    static float ReadIniFloat(const wchar_t* key, float fallback)
    {
        wchar_t defBuf[32] = {};
        _snwprintf_s(defBuf, _TRUNCATE, L"%.1f", fallback);

        wchar_t buf[32] = {};
        GetPrivateProfileStringW(L"Network", key, defBuf, buf, 32, IniPath().c_str());

        // wcstod rather than _wtof so a malformed value is distinguishable from
        // a legitimate 0 ("leave the engine default alone").
        wchar_t* end = nullptr;
        const double v = wcstod(buf, &end);
        if (end == buf)
        {
            ModLoaderLogger::LogWarn(
                L"[NetTimeout] [Network] %s in modloader.ini is not a number ('%s') -- using %.1f",
                key, buf, fallback);
            return fallback;
        }

        const float f = static_cast<float>(v);
        if (f == 0.0f)
            return 0.0f;   // explicit opt-out

        if (f < kMinTimeout || f > kMaxTimeout)
        {
            ModLoaderLogger::LogWarn(
                L"[NetTimeout] [Network] %s = %.1f is outside %.0f..%.0f seconds -- using %.1f",
                key, f, kMinTimeout, kMaxTimeout, fallback);
            return fallback;
        }
        return f;
    }

    void Initialize()
    {
        g_connectionTimeout     = ReadIniFloat(L"ConnectionTimeout",        kDefaultConnectionTimeout);
        g_initialConnectTimeout = ReadIniFloat(L"InitialConnectTimeout",    kDefaultInitialConnectTimeout);

        if (g_connectionTimeout == 0.0f && g_initialConnectTimeout == 0.0f)
        {
            ModLoaderLogger::LogInfo(
                L"[NetTimeout] Disabled by config -- the engine's own net timeouts are left alone");
            return;
        }

        ModLoaderLogger::LogInfo(
            L"[NetTimeout] Net timeouts configured: ConnectionTimeout=%.1fs InitialConnectTimeout=%.1fs "
            L"(0 = leave engine default). Both ends of a session need this for it to help.",
            g_connectionTimeout, g_initialConnectTimeout);
    }

    void Shutdown()
    {
        g_lastAppliedDriver = nullptr;
        g_warnedNoDriver    = false;
    }

    // The whole of the engine interaction, kept in one SEH-guarded helper with no
    // C++ objects in scope (C2712). Returns the driver it wrote to, or null.
    //
    // Writes are conditional on the value differing so the steady state is a
    // compare, not a store -- this runs every frame for the whole session.
    static void* ApplySEH(float connTimeout, float initialTimeout, float* outApplied)
    {
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world || !world->NetDriver) return nullptr;

            SDK::UNetDriver* driver = world->NetDriver;

            if (connTimeout != 0.0f && driver->ConnectionTimeout != connTimeout)
                driver->ConnectionTimeout = connTimeout;

            if (initialTimeout != 0.0f && driver->InitialConnectTimeout != initialTimeout)
                driver->InitialConnectTimeout = initialTimeout;

            *outApplied = driver->ConnectionTimeout;
            return driver;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    void Tick()
    {
        if (g_connectionTimeout == 0.0f && g_initialConnectTimeout == 0.0f) return;

        float applied = 0.0f;
        void* driver  = ApplySEH(g_connectionTimeout, g_initialConnectTimeout, &applied);

        if (!driver)
        {
            // No net driver is the normal state in the main menu, so this is not
            // a warning and it is not repeated. Clearing the applied-driver
            // marker is what makes the next driver log its own line.
            if (!g_warnedNoDriver)
            {
                ModLoaderLogger::LogTrace(
                    L"[NetTimeout] No net driver yet -- nothing to apply to");
                g_warnedNoDriver = true;
            }
            g_lastAppliedDriver = nullptr;
            return;
        }

        g_warnedNoDriver = false;

        if (driver != g_lastAppliedDriver)
        {
            g_lastAppliedDriver = driver;
            ModLoaderLogger::LogInfo(
                L"[NetTimeout] Applied to net driver %p: ConnectionTimeout=%.1fs "
                L"InitialConnectTimeout=%.1fs",
                driver, g_connectionTimeout, g_initialConnectTimeout);
        }
    }

    float GetConnectionTimeout()        { return g_connectionTimeout; }
    float GetInitialConnectTimeout()    { return g_initialConnectTimeout; }

    float GetAppliedConnectionTimeout()
    {
        float applied = 0.0f;
        // Pass 0/0 so this only reads -- a diagnostic getter must not write.
        if (!ApplySEH(0.0f, 0.0f, &applied)) return 0.0f;
        return applied;
    }

    void SetConnectionTimeout(float seconds)
    {
        if (seconds != 0.0f && (seconds < kMinTimeout || seconds > kMaxTimeout))
        {
            ModLoaderLogger::LogWarn(
                L"[NetTimeout] Ignoring ConnectionTimeout=%.1f -- outside %.0f..%.0f seconds",
                seconds, kMinTimeout, kMaxTimeout);
            return;
        }

        g_connectionTimeout = seconds;
        g_lastAppliedDriver = nullptr;   // force one log line for the new value

        wchar_t buf[32] = {};
        _snwprintf_s(buf, _TRUNCATE, L"%.1f", seconds);
        WritePrivateProfileStringW(L"Network", L"ConnectionTimeout", buf, IniPath().c_str());

        ModLoaderLogger::LogInfo(
            L"[NetTimeout] ConnectionTimeout set to %.1fs (applies on the next tick)", seconds);
    }
}

#else  // generic build -- no SDK, nothing to apply to

namespace NetTimeout
{
    void  Initialize() {}
    void  Shutdown()   {}
    void  Tick()       {}
    float GetConnectionTimeout()        { return 0.0f; }
    float GetInitialConnectTimeout()    { return 0.0f; }
    float GetAppliedConnectionTimeout() { return 0.0f; }
    void  SetConnectionTimeout(float)   {}
}

#endif
