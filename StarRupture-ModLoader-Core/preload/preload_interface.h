#pragma once

#include <windows.h>
#include <cstdint>

#include "plugins/plugin_interface.h"

// ---------------------------------------------------------------------------
// StarRupture ModLoader -- preload plugin interface
//
// A preload plugin is a DLL in ModLoader\Preload\ that runs during Stage 1 of
// loader startup: after the version check and the loader's own pattern
// preflight, and BEFORE the game's executable entry point has run at all.
//
// That is the whole reason it exists. An ordinary plugin's PluginInit happens
// once the engine is up, which is far too late to patch anything the engine
// touches on its way there. At preload time:
//
//   * The game .exe is mapped and can be scanned and patched.
//   * The CRT static initialisers have NOT run. GMalloc does not exist.
//     FEngineLoop::PreInit has not run. No UObject, no FName, no GConfig,
//     no GEngine, no world.
//   * The game's main thread is parked and will not start until every preload
//     plugin has been given its turn.
//
// SO: RESOLVE, PATCH, AND RETURN. Do not touch the engine, do not allocate
// through it, do not start threads that assume it exists, and do not block --
// the whole game is waiting on you, with a hard 180-second ceiling before the
// loader gives up and lets it boot regardless. Your detours fire later, on the
// game thread, when the engine is running.
//
// ---------------------------------------------------------------------------
// Refusal is cheap and the game never pays for it
// ---------------------------------------------------------------------------
//
// A preload plugin that fails anything -- wrong interface version, wrong build
// target, a pattern that does not resolve, a pattern that resolves twice, a
// pattern that resolves to the wrong kind of thing, a crash in any of its
// entry points -- is unloaded, reported, and skipped. The loader carries on
// and the game boots normally.
//
// This is the opposite of the loader's own preflight, which disables the whole
// mod loader when one of ITS patterns breaks. An outdated preload plugin is
// therefore safe to leave installed across a game update: it stops working, it
// says so, and nothing else changes.
//
// ---------------------------------------------------------------------------
// Two phases, and the split is not decorative
// ---------------------------------------------------------------------------
//
//   PreloadScan(self, scanner)   -- resolve every address you need. Nothing
//                                   else. `patch` is not available here.
//   PreloadInit(self, patch)     -- install. Only ever called when every
//                                   pattern in PreloadScan resolved cleanly.
//
// Splitting them is what makes refusal safe. If a plugin installed a detour
// while resolving and a later pattern then missed, the loader would have to
// free a module that has already written a jump into game code -- and it
// cannot undo that, or even find out it happened. So the interface simply does
// not hand you the means: there is no patch table during the scan.
//
// The scanner is the same IPluginHookScanner ordinary plugins get, with the
// same rules (see plugin_interface.h): a pattern must resolve to exactly one
// address, and with a PluginScanKind declared it must resolve to the right
// KIND of address. Failures land in the same report, shown in the same failure
// window and printed by the same `hookfailures` console command.
//
// ---------------------------------------------------------------------------
// Minimal plugin
// ---------------------------------------------------------------------------
//
//   static PreloadInfo s_info = {
//       PRELOAD_INTERFACE_VERSION, "MyPreload", "1.0.0", "me",
//       "Patches a thing before the engine starts",
//       PRELOAD_TARGET_CLIENT, 100
//   };
//   static uintptr_t s_target = 0;
//
//   extern "C" __declspec(dllexport) PreloadInfo* GetPreloadInfo() { return &s_info; }
//
//   extern "C" __declspec(dllexport)
//   void PreloadScan(IPluginSelf* self, IPluginHookScanner* scanner)
//   {
//       PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
//       req.hookName = "FEngineLoop::PreInit";
//       req.pattern  = "48 89 5C 24 ?? 57 48 83 EC ??";
//       req.kind     = PLUGIN_SCAN_FUNCTION_START;
//       s_target = scanner->Resolve(self, &req);
//   }
//
//   extern "C" __declspec(dllexport)
//   bool PreloadInit(IPluginSelf* self, IPreloadPatch* patch)
//   {
//       return patch->InstallHook(self, "FEngineLoop::PreInit", s_target,
//                                 &MyDetour, (void**)&s_original);
//   }
//
//   extern "C" __declspec(dllexport) void PreloadShutdown() {}   // optional
// ---------------------------------------------------------------------------

// Versioned independently of PLUGIN_INTERFACE_VERSION. A preload plugin should
// not need rebuilding every time the (much larger, much busier) plugin API
// gains a field it does not use.
#define PRELOAD_INTERFACE_VERSION_MIN 1
#define PRELOAD_INTERFACE_VERSION_MAX 1
#define PRELOAD_INTERFACE_VERSION     1

#define PRELOAD_TARGET_CLIENT 1
#define PRELOAD_TARGET_SERVER 2

struct PreloadInfo
{
    // Must be PRELOAD_INTERFACE_VERSION. Outside [MIN, MAX] the DLL is skipped.
    int         interfaceVersion;

    const char* name;         // shown in logs, the console and the failure report
    const char* version;
    const char* author;
    const char* description;

    // PRELOAD_TARGET_CLIENT or PRELOAD_TARGET_SERVER. A mismatch is skipped --
    // the client and the dedicated server are different executables with
    // different code at different addresses.
    int         target;

    // Lower runs first; equal priorities run in filename order. It matters when
    // two preload plugins patch the same function: whoever installs second
    // detours whatever the first one left behind. 100 is a sensible default for
    // "no opinion".
    int         priority;
};

// ---------------------------------------------------------------------------
// IPreloadPatch -- everything a preload plugin is allowed to do to the game.
//
// Deliberately small. At preload time there is no engine to expose, and every
// function here works on raw bytes in a mapped image.
//
// Hooks are owned by the LOADER, not by the plugin: InstallHook registers the
// detour under a name, and the loader removes every one of a plugin's hooks if
// that plugin is ever unloaded. A preload plugin cannot be reloaded mid-session
// (its window is gone), but the loader still unhooks on shutdown so nothing
// points into an unmapped module while the process is alive.
// ---------------------------------------------------------------------------
struct IPreloadPatch
{
    // --- Where things are ---------------------------------------------------

    HMODULE     (*GetGameModule)();
    uintptr_t   (*GetGameModuleBase)();
    size_t      (*GetGameModuleSize)();

    // The game's ProductVersion string, as the loader's own version check reads
    // it. Empty string if it could not be determined.
    const char* (*GetGameVersion)();

    // The mod loader's build tag ("dev" on local builds).
    const char* (*GetLoaderBuildTag)();

    // --- Raw memory ---------------------------------------------------------
    //
    // All three handle page protection themselves and refuse an address outside
    // a loaded module, so a bad offset is a false return rather than an access
    // violation before the game has even started.

    bool (*ReadBytes)(uintptr_t address, void* dest, size_t size);
    bool (*WriteBytes)(uintptr_t address, const void* source, size_t size);
    bool (*Nop)(uintptr_t address, size_t size);

    // --- Detours ------------------------------------------------------------

    // Installs a 14-byte absolute JMP at target and hands back a trampoline
    // that calls the original. `name` identifies the hook in logs and in
    // RemoveHook; it does not have to match the name used during the scan, but
    // it reads better when it does.
    //
    // Returns false and logs if the target is not hookable -- which includes an
    // address of 0, so passing through a pattern that did not resolve fails
    // cleanly rather than patching address zero.
    bool (*InstallHook)(const IPluginSelf* self, const char* name, uintptr_t target,
                        void* detour, void** outOriginal);

    // Removes a hook this plugin installed under that name. Rarely needed --
    // the loader removes them all at shutdown.
    bool (*RemoveHook)(const IPluginSelf* self, const char* name);
};

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

#define PRELOAD_GET_INFO_FUNC_NAME  "GetPreloadInfo"
#define PRELOAD_SCAN_FUNC_NAME      "PreloadScan"
#define PRELOAD_INIT_FUNC_NAME      "PreloadInit"
#define PRELOAD_SHUTDOWN_FUNC_NAME  "PreloadShutdown"

// Required.
typedef PreloadInfo* (*GetPreloadInfoFunc)();

// Optional -- a preload plugin that resolves no patterns does not need one, and
// its absence is not an error. `self->hooks`, `self->config` and the patch
// table are all unavailable during this call, by design.
typedef void (*PreloadScanFunc)(IPluginSelf* self, IPluginHookScanner* scanner);

// Required. Return false to decline quietly (the DLL is unloaded, nothing is
// reported as a failure -- use the scanner's ReportFailure during the scan if
// you want it on the record).
typedef bool (*PreloadInitFunc)(IPluginSelf* self, IPreloadPatch* patch);

// Optional. Called at loader shutdown, after the loader has already removed
// every hook this plugin installed.
typedef void (*PreloadShutdownFunc)();
