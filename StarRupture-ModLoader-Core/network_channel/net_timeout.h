#pragma once

// ============================================================
// Net connection timeout override
//
// Raises UNetDriver::ConnectionTimeout / InitialConnectTimeout above the
// engine's defaults, on both the authority and the client, so a peer is not
// dropped while the other end is busy.
//
// WHY THIS EXISTS
// A player joining a session that then travels sits through a map load during
// which the host's game thread does not tick. Nothing is sent -- not even
// keepalives, because UNetConnection::Tick is what emits them -- so the client
// sees pure silence for the length of the load and its own timeout fires. One
// observed case ran 80 s ("LogNet: Very long time between ticks. DeltaTime:
// 79.94") against a stock 60 s ConnectionTimeout.
//
// WHY IT IS NOT A LAUNCH ARGUMENT
// The engine does read these -- UNetDriver::InitBase parses them out of the FURL
// it is initialised with:
//
//     if (const TCHAR* Opt = URL.GetOption(TEXT("ConnectionTimeout="), nullptr))
//         if (float v = _wtof(Opt); v != 0.f) ConnectionTimeout = v;
//
// but there is no way to get an option INTO that URL from outside the process on
// this build. Stock UGameEngine::Init turns the command line into the initial
// FURL; in this shipping binary that block is absent entirely (no
// FCommandLine::Get, no FURL, no Browse), and even in stock UE it is compiled out
// under UE_BUILD_SHIPPING. The join URL is built in-process from the session's
// connect string. So `?ConnectionTimeout=300` in Steam launch options parses
// as nothing at all, and writing the field is the only route.
//
// Note this is NOT the command line being ignored in general -- that works fine
// here (the -console and -log handling depends on it). It is specifically the
// URL that is unreachable.
//
// WHAT IT CANNOT DO
// Timeouts are evaluated independently on each side against that side's own
// clock, so the side that gives up is the side that needs the higher value. A
// vanilla client cannot be helped by anything the host does. This is therefore a
// mitigation for sessions where both ends run the loader, not a general fix --
// and the real fix for a long stall is the host not stalling.
// ============================================================

namespace NetTimeout
{
    // Reads modloader.ini. Safe to call before the engine has a net driver.
    void Initialize();

    void Shutdown();

    // Idempotent re-apply. Driven from NetworkChannel's engine-tick callback
    // rather than from a one-shot at engine init, because the net driver is
    // created and destroyed repeatedly across travel and every new one starts at
    // the engine default.
    void Tick();

    // Configured values in seconds. 0 means "leave the engine default alone".
    float GetConnectionTimeout();
    float GetInitialConnectTimeout();

    // The value the live net driver currently holds, or 0 if there is no driver.
    // For diagnostics -- this is what the engine will actually time out on.
    float GetAppliedConnectionTimeout();

    // Live change plus persist to modloader.ini. Takes effect on the next tick.
    void SetConnectionTimeout(float seconds);
}
