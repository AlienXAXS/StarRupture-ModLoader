// ---------------------------------------------------------------------------
// PreloadTest -- an end-to-end check of the preload phase, in one DLL.
//
// It hooks FEngineLoop::Init and logs. That is deliberately the least
// interesting thing it could do, because the point is not the hook -- it is
// that the hook is possible at all, and that getting there exercises both of
// the subsystems that are easy to break and hard to notice breaking.
//
// WHY THIS TARGET
//
// FEngineLoop::Init is the one function that tests everything at once:
//
//   1. The loader ALREADY hooks it (hooks/game/engine_init/), and it installs
//      that hook in Stage 1 BEFORE the preload phase runs. So by the time this
//      plugin scans, the first 14+ bytes of the function have been replaced
//      with a JMP stub -- and the AOB below is anchored on exactly those bytes.
//
//      If ScanValidation's patch overlay works, the pattern resolves anyway,
//      because the scanner reads through the loader's own hooks and sees the
//      image as it shipped. If it does not work, this plugin is refused with
//      "pattern not found" and the failure is loud and obvious.
//
//   2. Installing here means TWO detours on one address. If Hooks::Broker
//      works, both run: the loader's engine-init hook first (it was installed
//      first), then ours, then the real function. If it does not, the game
//      crashes or the engine never initialises.
//
//   3. It is called exactly once, early, so a working test writes one line
//      rather than flooding the log.
//
// The AOB is lifted verbatim from ScanPatterns::FEngineLoop_Init in
// hooks/game/scan_patterns.h rather than being dumped fresh. That is the point:
// it is already verified against the current game build by the loader's own
// preflight, so if THIS resolve fails while the loader booted fine, the fault
// is in the preload path or the overlay, not in the pattern. A freshly dumped
// AOB would make a failure ambiguous.
//
// Both build targets use the same pattern -- FEngineLoop::Init sits outside the
// client/server conditionals in scan_patterns.h.
//
// See Tests/README.md for how to build and run it.
// ---------------------------------------------------------------------------

#include "preload/preload_interface.h"

#include <cstdio>

// ---------------------------------------------------------------------------

#if defined(MODLOADER_CLIENT_BUILD)
#define PRELOAD_TEST_TARGET PRELOAD_TARGET_CLIENT
#else
#define PRELOAD_TEST_TARGET PRELOAD_TARGET_SERVER
#endif

static PreloadInfo s_info = {
    PRELOAD_INTERFACE_VERSION,
    "PreloadTest",
    "1.0.0",
    "modloader",
    "Hooks FEngineLoop::Init from the preload phase and logs when it fires",
    PRELOAD_TEST_TARGET,
    100
};

// Lifted from ScanPatterns::FEngineLoop_Init -- see the header comment.
static const char* kEngineLoopInitPattern =
    "4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? "
    "48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? 48 8D 15";

// int32_t FEngineLoop::Init(FEngineLoop* this)
typedef int32_t(__fastcall* FEngineLoop_Init_t)(void* self);

static uintptr_t          s_initAddress  = 0;
static FEngineLoop_Init_t s_originalInit = nullptr;
static IPluginSelf*       s_self         = nullptr;

// Runs on the game's main thread, once, as the engine comes up -- long after
// everything else in this file has finished.
//
// Note what `s_originalInit` actually is: a broker thunk, not FEngineLoop::Init.
// Calling it continues down the chain (to the real function, since we are the
// last link), which is exactly how it should be used. Do not inspect it.
static int32_t __fastcall InitDetour(void* self)
{
    if (s_self && s_self->logger)
    {
        s_self->logger->Info(s_self,
            "FEngineLoop::Init reached -- preload detour fired, chained behind the loader's own hook");
    }

    const int32_t result = s_originalInit(self);

    if (s_self && s_self->logger)
        s_self->logger->Info(s_self, "FEngineLoop::Init returned %d", result);

    return result;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) PreloadInfo* GetPreloadInfo()
{
    return &s_info;
}

extern "C" __declspec(dllexport)
void PreloadScan(IPluginSelf* self, IPluginHookScanner* scanner)
{
    PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
    request.hookName = "FEngineLoop::Init";
    request.pattern  = kEngineLoopInitPattern;

    // FUNCTION_START rather than ANY on purpose. The address has to be a real
    // function entry with room for a detour, and saying so means a pattern that
    // drifts into the middle of something after a game update is refused here
    // rather than detoured.
    request.kind     = PLUGIN_SCAN_FUNCTION_START;

    s_initAddress = scanner->Resolve(self, &request);

    // No error handling, deliberately: a zero return has already been recorded
    // against this plugin, PreloadInit will never be called, and the DLL is
    // about to be freed. There is nothing useful to add here.
}

extern "C" __declspec(dllexport)
bool PreloadInit(IPluginSelf* self, IPreloadPatch* patch)
{
    s_self = self;

    self->logger->Info(self, "preload test starting -- game %s, loader %s, module base 0x%llX",
                       patch->GetGameVersion(),
                       patch->GetLoaderBuildTag(),
                       static_cast<unsigned long long>(patch->GetGameModuleBase()));

    self->logger->Info(self, "FEngineLoop::Init resolved to exe+0x%llX",
                       static_cast<unsigned long long>(s_initAddress - patch->GetGameModuleBase()));

    if (!patch->InstallHook(self, "FEngineLoop::Init", s_initAddress,
                            &InitDetour, reinterpret_cast<void**>(&s_originalInit)))
    {
        self->logger->Error(self, "could not install the FEngineLoop::Init hook -- standing down");
        return false;
    }

    self->logger->Info(self, "hook installed; expect a line from the detour when the engine starts");
    return true;
}

extern "C" __declspec(dllexport)
void PreloadShutdown()
{
    // The loader has already removed the hook by the time this runs, so there is
    // nothing to undo -- only the pointer the detour reads, which must not be
    // left dangling if anything is still in flight.
    s_self = nullptr;
}
