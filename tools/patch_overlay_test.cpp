// Standalone exercise for PatchOverlay::Snapshot (memory_scanner/patch_overlay.h).
//
// Builds a fake "image" in a heap buffer, declares some of it patched, and
// checks that scanning reports what the UNPATCHED buffer would have contained.
// The logger, the scan cache and the broker are stubbed, so only scanner.cpp,
// image_info.cpp and patch_overlay.cpp are actually under test.
//
// Worth having because the overlay silently changes the answer every plugin
// resolve gets, and its window arithmetic has three cases that are easy to get
// subtly wrong and impossible to notice at runtime: a match that straddles the
// edge of a patch, a match invented by our own JMP stub, and a match found
// twice because two patches sit close enough to share a window.
//
// Build and run (from the repo root, in a VS developer prompt):
//
//   cl /nologo /std:c++20 /EHsc /DNOMINMAX /DUNICODE /D_UNICODE ^
//      /D_CRT_SECURE_NO_WARNINGS /I StarRupture-ModLoader-Core ^
//      /Fe:patch_overlay_test.exe tools\patch_overlay_test.cpp ^
//      StarRupture-ModLoader-Core\memory_scanner\patch_overlay.cpp ^
//      StarRupture-ModLoader-Core\memory_scanner\scanner.cpp ^
//      StarRupture-ModLoader-Core\memory_scanner\image_info.cpp
//
// Exit code is the number of failed checks.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "memory_scanner/patch_overlay.h"
#include "memory_scanner/scanner.h"
#include "hooks/hook_broker.h"
#include "memory_scanner/scan_cache.h"

// --- stubs -------------------------------------------------------------------

namespace ModLoaderLogger
{
    void LogMessage(const wchar_t*, ...) {}
    void LogTrace(const wchar_t*, ...)   {}
    void LogDebug(const wchar_t*, ...)   {}
    void LogInfo(const wchar_t*, ...)    {}
    void LogWarn(const wchar_t*, ...)    {}
    void LogError(const wchar_t*, ...)   {}
}

std::wstring GetGameVersionString() { return L""; }

namespace ScanCache
{
    bool TryGet(const std::wstring&, const std::string&, Entry&) { return false; }
    void Store(const std::wstring&, const std::string&, const Entry&) {}
}

static std::vector<Hooks::Broker::PatchedRange> g_fakeRanges;

namespace Hooks::Broker
{
    std::vector<PatchedRange> GetPatchedRanges() { return g_fakeRanges; }
}

// --- harness -----------------------------------------------------------------

static int g_failures = 0;

static void Check(bool condition, const char* what)
{
    printf("%-72s %s\n", what, condition ? "ok" : "FAIL");
    if (!condition) ++g_failures;
}

// The bytes we pretend the shipped binary contained.
static std::vector<uint8_t> g_original;
// The same buffer with our JMP stubs written over it -- what a live scan sees.
static std::vector<uint8_t> g_live;

static const uint8_t kStub[14] = {
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
    0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00
};

static void Patch(size_t offset)
{
    Hooks::Broker::PatchedRange range;
    range.address = reinterpret_cast<uintptr_t>(g_live.data()) + offset;
    range.originalBytes.assign(g_original.begin() + offset,
                               g_original.begin() + offset + sizeof(kStub));
    g_fakeRanges.push_back(std::move(range));

    memcpy(g_live.data() + offset, kStub, sizeof(kStub));
}

static std::vector<uintptr_t> Scan(const char* pattern)
{
    const PatchOverlay::Snapshot overlay;
    const auto parsed = Scanner::ParsePattern(pattern);

    std::vector<uintptr_t> out;
    overlay.Collect(reinterpret_cast<uintptr_t>(g_live.data()), g_live.size(),
                    parsed, 64, 4096, out);

    const uintptr_t base = reinterpret_cast<uintptr_t>(g_live.data());
    for (uintptr_t& address : out)
        address -= base;
    return out;
}

int main()
{
    // A 4 KB buffer of filler, with a recognisable "function prologue" planted
    // at three offsets. 0x90 filler cannot collide with the patterns below.
    g_original.assign(4096, 0x90);

    const uint8_t prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    const size_t sites[] = { 0x100, 0x400, 0x800 };
    for (size_t site : sites)
        memcpy(g_original.data() + site, prologue, sizeof(prologue));

    g_live = g_original;

    // ---- no patches: overlay must agree with a plain scan -------------------
    {
        auto hits = Scan("48 89 5C 24 ?? 57 48 83 EC 20");
        Check(hits.size() == 3, "unpatched: finds all three sites");
        Check(hits.size() == 3 && hits[0] == 0x100 && hits[1] == 0x400 && hits[2] == 0x800,
              "unpatched: in address order");
    }

    // ---- one site hooked: the masked match must come back -------------------
    Patch(0x400);
    {
        auto hits = Scan("48 89 5C 24 ?? 57 48 83 EC 20");
        Check(hits.size() == 3, "one site hooked: still finds all three");
        Check(hits.size() == 3 && hits[1] == 0x400, "one site hooked: masked site reported at its real address");
    }

    // ---- a pattern matching our own stub must NOT be reported ---------------
    {
        auto hits = Scan("FF 25 00 00 00 00");
        Check(hits.empty(), "stub bytes are invisible: FF 25 00 00 00 00 finds nothing");
    }

    // ---- a match that STRADDLES the patch boundary --------------------------
    // Two bytes of the pattern sit before the patch, the rest inside it.
    {
        auto hits = Scan("90 90 48 89 5C 24");
        bool found400 = false;
        for (uintptr_t h : hits) found400 = found400 || (h == 0x3FE);
        Check(found400, "straddling match spanning into a patched range is found");
    }

    // ---- all three hooked ---------------------------------------------------
    Patch(0x100);
    Patch(0x800);
    {
        auto hits = Scan("48 89 5C 24 ?? 57 48 83 EC 20");
        Check(hits.size() == 3, "all three hooked: all three still found");
        Check(hits.size() == 3 && hits[0] == 0x100 && hits[1] == 0x400 && hits[2] == 0x800,
              "all three hooked: correct addresses, no duplicates");
    }

    // ---- two patches close enough that one window covers both ---------------
    g_fakeRanges.clear();
    g_live = g_original;
    memcpy(g_live.data() + 0x600, prologue, sizeof(prologue));
    memcpy(g_original.data() + 0x600, prologue, sizeof(prologue));
    memcpy(g_original.data() + 0x610, prologue, sizeof(prologue));
    g_live = g_original;
    Patch(0x600);
    Patch(0x610);
    {
        auto hits = Scan("48 89 5C 24 ?? 57 48 83 EC 20");
        int near600 = 0;
        for (uintptr_t h : hits) if (h == 0x600 || h == 0x610) ++near600;
        Check(near600 == 2, "adjacent patches: both reconstructed, counted once each");
    }

    // ---- MatchesAt, the scan-cache revalidation path ------------------------
    {
        const PatchOverlay::Snapshot overlay;
        const auto parsed = Scanner::ParsePattern("48 89 5C 24 ?? 57 48 83 EC 20");
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_live.data());
        Check(overlay.MatchesAt(base + 0x600, parsed), "MatchesAt: sees through a patch at a cached offset");
        Check(!overlay.MatchesAt(base + 0x601, parsed), "MatchesAt: does not match one byte off");
        Check(overlay.MatchesAt(base + 0x100, parsed), "MatchesAt: unpatched site still matches");
    }

    printf("\n%s\n", g_failures == 0 ? "all overlay checks passed" : "OVERLAY CHECKS FAILED");
    return g_failures;
}
