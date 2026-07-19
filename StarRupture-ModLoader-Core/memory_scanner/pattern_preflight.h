#pragma once

// Startup preflight for the modloader's own scan patterns.
//
// Scans every entry in ScanPatterns::PreflightRegistry against the main
// module before any hook is installed. Every failure is logged to the
// modloader log. If any *required* pattern is missing, the modloader must
// not install any hooks at all -- callers check the return value and abort
// initialization entirely so the game never runs partially hooked.
//
// A successful preflight also warms the scan cache, so the per-hook scans
// that follow are near-free cache hits.

namespace PatternPreflight
{
    // Returns true if every required modloader pattern was found.
    bool VerifyAllPatterns();
}
