// PluginPanelRegistry and PluginWidgetRegistry test: ForgetModule, and unloads
// that race the render thread.
//
// A console program: no game, no plugin DLLs. The registries are the loader's
// own source files; test_doubles.cpp replaces only what they call out to.
//
// Two "plugins" are needed, and a module is whatever an address lives in. This
// executable is one: its own functions are plugin A's panel and callback.
// kernel32.dll is the other: a function address from it stands in for plugin B's
// renderFn and callback. They are GetLastError and GetCurrentThreadId, which
// take no arguments, so calling them with a panel's arguments is harmless.
#include "UI/plugin_panel_registry.h"
#include "UI/plugin_widget_registry.h"
#include "UI/plugin_call_tracker.h"
#include "plugins/plugin_guard.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // GImGui, to see what a crashed render left behind
#include "test_doubles.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

namespace Registry = UI::PluginPanelRegistry;

// ---------------------------------------------------------------------------
// Plugin A: lives in this executable.
// ---------------------------------------------------------------------------

static int g_closedA = 0;

static void RenderA(IModLoaderImGui*) {}
static void ClosedA(PanelHandle) { ++g_closedA; }

static const PluginPanelDesc kPanelA = { "A", "Panel A", RenderA };

// ---------------------------------------------------------------------------
// Plugin B: lives in kernel32.dll (see above).
// ---------------------------------------------------------------------------

static HMODULE ModuleB()
{
    return GetModuleHandleW(L"kernel32.dll");
}

static PluginImGuiRenderCallback RenderB()
{
    return reinterpret_cast<PluginImGuiRenderCallback>(GetProcAddress(ModuleB(), "GetLastError"));
}

static PluginPanelClosedCallback ClosedB()
{
    return reinterpret_cast<PluginPanelClosedCallback>(GetProcAddress(ModuleB(), "GetCurrentThreadId"));
}

static int PanelCount()
{
    int registered = 0;
    Registry::GetPanelCounts(&registered, nullptr);
    return registered;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The unloading plugin's panels are dropped, the other plugin's are kept, and
// the same title can be registered again afterwards (a reloaded plugin does).
static void Test_PanelsPurgedByModule()
{
    const PluginPanelDesc panelB = { "B", "Panel B", RenderB() };

    PanelHandle a = Registry::RegisterPanel(&kPanelA);
    PanelHandle b = Registry::RegisterPanel(&panelB);
    CHECK(a && b);
    CHECK(PanelCount() == 2);

    Registry::ForgetModule(GetModuleHandleW(nullptr)); // plugin A unloads

    CHECK(PanelCount() == 1);
    CHECK(Registry::RegisterPanel(&panelB) == nullptr); // B is still registered: duplicate title
    PanelHandle a2 = Registry::RegisterPanel(&kPanelA);  // A is gone: registers afresh
    CHECK(a2 != nullptr);
    CHECK(PanelCount() == 2);

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    Registry::ForgetModule(ModuleB());
    CHECK(PanelCount() == 0);
}

// A panel that was open when its plugin went away is dropped quietly: no
// callback into the module, and a handle the plugin still holds is harmless.
static void Test_OpenPanelAndStaleHandle()
{
    g_closedA = 0;
    Registry::RegisterOnPanelWindowClosed(ClosedA);

    PanelHandle a = Registry::RegisterPanel(&kPanelA);
    Registry::SetPanelOpen(a);
    CHECK(Registry::AnyPanelOpen());

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    CHECK(!Registry::AnyPanelOpen());
    CHECK(PanelCount() == 0);

    Registry::SetPanelClose(a);   // stale handle: ignored
    Registry::SetPanelOpen(a);
    Registry::UnregisterPanel(a);
    CHECK(PanelCount() == 0);
    CHECK(g_closedA == 0);
}

// Panel-closed callbacks have an owner too: the unloading plugin's is dropped
// and never called again, the other plugin's stays registered. Closing plugin
// B's panel fires every callback still registered. B's own callback is the
// harmless GetCurrentThreadId; A's is the one that would run freed code.
static void Test_ClosedCallbacksPurgedByModule()
{
    g_closedA = 0;
    const PluginPanelDesc panelB = { "B", "Panel B", RenderB() };

    Registry::RegisterOnPanelWindowClosed(ClosedA);
    Registry::RegisterOnPanelWindowClosed(ClosedB());
    PanelHandle b = Registry::RegisterPanel(&panelB);

    // Control: with both registered, closing B's panel reaches A's callback.
    Registry::SetPanelOpen(b);
    Registry::SetPanelClose(b);
    CHECK(g_closedA == 1);

    Registry::ForgetModule(GetModuleHandleW(nullptr)); // plugin A unloads

    Registry::SetPanelOpen(b);
    Registry::SetPanelClose(b);
    CHECK(g_closedA == 1); // not called again

    Registry::UnregisterOnPanelWindowClosed(ClosedB());
    Registry::ForgetModule(ModuleB());
}

// Forgetting a module that owns nothing, or no module, changes nothing.
static void Test_ForgetOfUnrelatedModuleIsHarmless()
{
    PanelHandle a = Registry::RegisterPanel(&kPanelA);
    CHECK(a != nullptr);

    Registry::ForgetModule(nullptr);
    Registry::ForgetModule(ModuleB());
    CHECK(PanelCount() == 1);

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    CHECK(PanelCount() == 0);
}

// Widgets hold the same kind of pointers and are purged the same way. There is
// no widget count to read, so a duplicate name shows what is still registered.
static void Test_WidgetsPurgedByModule()
{
    const PluginWidgetDesc widgetA = { "Widget A", RenderA, nullptr };
    const PluginWidgetDesc widgetB = { "Widget B", RenderB(), nullptr };

    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetA) != nullptr);
    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetB) != nullptr);

    UI::PluginWidgetRegistry::ForgetModule(GetModuleHandleW(nullptr)); // plugin A unloads

    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetB) == nullptr); // B kept
    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetA) != nullptr); // A gone

    UI::PluginWidgetRegistry::ForgetModule(GetModuleHandleW(nullptr));
    UI::PluginWidgetRegistry::ForgetModule(ModuleB());
    CHECK(UI::PluginWidgetRegistry::RegisterWidget(&widgetB) != nullptr);
    UI::PluginWidgetRegistry::ForgetModule(ModuleB());
}

// The tests above prove the purge works; this one proves the loader calls it.
// plugin_manager.cpp cannot be linked into a console program (it is the plugin
// loader), so its three paths that unmap a plugin -- unload all, unload one,
// reload -- are read as source. Each must call both registries' ForgetModule,
// on the module it frees, before FreeLibrary, since afterwards a panel's or
// widget's renderFn is freed memory. Delete any one call and this fails.
static std::string StripLineComments(const std::string& src)
{
    std::string out;
    for (size_t pos = 0; pos < src.size();)
    {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) eol = src.size();
        std::string line = src.substr(pos, eol - pos);
        const size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        out += line;
        out += '\n';
        pos = eol + 1;
    }
    return out;
}

// Text of the function whose definition starts with signature, up to its
// closing brace (a tab-indented "}" line; the functions sit in a namespace).
// A forward declaration (the signature then ';') is not the definition.
static std::string FunctionBody(const std::string& src, const char* signature)
{
    size_t start = src.find(signature);
    while (start != std::string::npos && src.compare(start + strlen(signature), 1, ";") == 0)
        start = src.find(signature, start + 1);
    if (start == std::string::npos) return "";
    const size_t end = src.find("\n\t}", start);
    return end == std::string::npos ? "" : src.substr(start, end - start);
}

static void Test_UnloadPathsPurgePanels()
{
    // <solution>\build\tests\panel_unload_tests.exe: the exe's name, then tests, then build.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    for (int up = 0; up < 3; ++up)
        path.erase(path.find_last_of(L"\\/"));
    path += L"\\StarRupture-ModLoader-Core\\plugins\\plugin_manager.cpp";

    std::ifstream in(path.c_str(), std::ios::binary);
    CHECK(in.good());
    const std::string src = StripLineComments(
        std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));

    const char* const kPaths[] =
    {
        "void UnloadAllPlugins()",
        "static bool ShutDownAndFree(LoadedPlugin& p, const wchar_t* action)",
        "bool ReloadPlugin(int index, std::string* refusal)",
    };
    for (const char* signature : kPaths)
    {
        printf("    %s\n", signature);
        const std::string body = FunctionBody(src, signature);
        CHECK(!body.empty());

        const size_t free = body.find("FreeLibrary(");
        CHECK(free != std::string::npos);
        if (free == std::string::npos) continue;

        // The module being freed, e.g. "p.hModule".
        const size_t argStart = free + strlen("FreeLibrary(");
        const std::string module = body.substr(argStart, body.find(')', argStart) - argStart);

        // The renders are waited out before the purges...
        const size_t wait = body.find("= WaitOutPluginRenders(" + module + ",");
        CHECK(wait != std::string::npos);

        // ...then everything the plugin registered goes, before the free.
        const size_t forget = body.find("ForgetRegistrations(", wait);
        CHECK(forget != std::string::npos);
        CHECK(forget < free);

        // ...and when the wait gives up, the overlay may still be in the
        // module, so the path must stop before FreeLibrary.
        const size_t refuse = body.find("if (!idle)", wait);
        CHECK(refuse != std::string::npos);
        CHECK(refuse < free);
    }

    // UNLOAD frees through ShutDownAndFree, checked above.
    CHECK(FunctionBody(src, "bool UnloadPlugin(int index, std::string* refusal)").find("ShutDownAndFree(") != std::string::npos);

    // A refused UNLOAD or RELOAD is decided before anything of the plugin is
    // touched: the refusal check comes ahead of the shutdown and the free.
    for (const char* signature : { "bool UnloadPlugin(int index, std::string* refusal)", "bool ReloadPlugin(int index, std::string* refusal)" })
    {
        const std::string body = FunctionBody(src, signature);
        const size_t refuse = body.find("ReloadPolicy::");
        CHECK(refuse != std::string::npos);
        for (const char* teardown : { "ShutDownAndFree(", "CallShutdownSEH(", "WaitOutPluginRenders(", "FreeLibrary(" })
        {
            const size_t at = body.find(teardown);
            if (at != std::string::npos)
                CHECK(refuse < at);
        }
    }

    // ForgetRegistrations purges both registries for the module being freed.
    const std::string forget = FunctionBody(src, "static void ForgetRegistrations(LoadedPlugin& p)");
    CHECK(forget.find("UI::PluginPanelRegistry::ForgetModule(p.hModule)") != std::string::npos);
    CHECK(forget.find("UI::PluginWidgetRegistry::ForgetModule(p.hModule)") != std::string::npos);
    CHECK(forget.find("ModLoaderLogger::ForgetPluginCallbacks(p.hModule)") != std::string::npos);

    // ...and that wait must come from both registries, with its answer used.
    const std::string wait = FunctionBody(src, "static bool WaitOutPluginRenders(");
    CHECK(wait.find("= UI::PluginPanelRegistry::ForgetModule(module)") != std::string::npos);
    CHECK(wait.find("= UI::PluginWidgetRegistry::ForgetModule(module)") != std::string::npos);
    CHECK(wait.find("return false;") != std::string::npos);
}

static std::string ReadSource(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return StripLineComments(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

// Every ForgetModule(HMODULE) and ForgetPlugin(const char*) a loader header
// declares is called from ForgetRegistrations or ForgetPluginCallbacks: a
// registry added later without being wired in would keep a freed plugin's
// callbacks. A list with no purge at all is not caught here; those are
// tested on the real list (the game-thread queue in KeyAndThreadTests).
static void Test_EveryForgetModuleIsCalledOnUnload()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path core = exe;
    core = core.parent_path().parent_path().parent_path() / L"StarRupture-ModLoader-Core";

    const std::string calls =
        FunctionBody(ReadSource(core / L"plugins" / L"plugin_manager.cpp"), "static void ForgetRegistrations(LoadedPlugin& p)") +
        FunctionBody(ReadSource(core / L"hooks" / L"hooks_interface.cpp"), "void ForgetPluginCallbacks(HMODULE module)");
    CHECK(!calls.empty());

    int declared = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(core))
    {
        if (entry.path().extension() != L".h" || entry.path().filename() == L"plugin_guard.h")
            continue;
        const std::string header = ReadSource(entry.path());
        for (const std::string purge : { "ForgetModule(HMODULE", "ForgetPlugin(const char*" })
        {
            const size_t decl = header.find(purge);
            if (decl == std::string::npos)
                continue;
            const std::string call = purge.substr(0, purge.find('(') + 1);   // "ForgetModule("

            // The innermost namespace the declaration sits in: "namespace Hooks::EngineTick" -> "EngineTick".
            const size_t ns = header.rfind("namespace ", decl);
            CHECK(ns != std::string::npos);
            if (ns == std::string::npos) continue;
            const size_t nameStart = ns + strlen("namespace ");
            std::string name = header.substr(nameStart, header.find_first_of(" \r\n{", nameStart) - nameStart);
            if (const size_t colon = name.rfind("::"); colon != std::string::npos)
                name.erase(0, colon + 2);

            ++declared;
            const bool called = calls.find(name + "::" + call) != std::string::npos;
            if (!called)
                printf("    not called on unload: %s::%s) (%ls)\n", name.c_str(), call.c_str(), entry.path().filename().c_str());
            CHECK(called);
        }
    }
    CHECK(declared >= 20);
}

// ---------------------------------------------------------------------------
// Render versus unload
//
// The render thread draws panels while the game thread unloads plugins. These
// run RenderPanelWindows on a second thread, hold it inside a panel's renderFn,
// and unload from a third, as the game does.
// ---------------------------------------------------------------------------

static HANDLE      g_inRender;           // set once RenderSlow is running
static HANDLE      g_letRenderFinish;    // RenderSlow returns when this is set
static PanelHandle g_slowPanel;
static bool        g_unregisterFromRender = false;
static bool        g_registerAfterHold    = false;

static void RenderSlow(IModLoaderImGui*)
{
    if (g_unregisterFromRender)
        Registry::UnregisterPanel(g_slowPanel);   // its own panel, on the render thread
    SetEvent(g_inRender);
    WaitForSingleObject(g_letRenderFinish, 10000);
    if (g_registerAfterHold)
        Registry::RegisterPanel(&kPanelA);   // while the unload is still waiting
}

static const PluginPanelDesc kSlowPanel = { "Slow", "Slow panel", RenderSlow };

static void OpenSlowPanel()
{
    g_slowPanel = Registry::RegisterPanel(&kSlowPanel);
    CHECK(g_slowPanel != nullptr);
    Registry::SetPanelOpen(g_slowPanel);
    TestDoubles::drawnTitles.clear();
    TestDoubles::loggedErrors = 0;
}

static void DrawPanels()
{
    Registry::RenderPanelWindows(nullptr);
}

// Runs call (by default, drawing the open panels) on a new thread and returns
// once it is held inside plugin code: RenderSlow or ClosedSlow.
static std::thread StartRender(void (*call)() = DrawPanels)
{
    ResetEvent(g_inRender);
    ResetEvent(g_letRenderFinish);
    std::thread render(call);
    CHECK(WaitForSingleObject(g_inRender, 5000) == WAIT_OBJECT_0);
    return render;
}

// Runs unload on its own thread while call is held in plugin code, lets it
// finish after holdMs, and returns how long unload took to return.
static ULONGLONG UnloadDuringRender(void (*unload)(), DWORD holdMs, void (*call)() = DrawPanels)
{
    std::thread render = StartRender(call);

    const ULONGLONG start = GetTickCount64();
    std::atomic<ULONGLONG> returned{ 0 };
    std::thread unloader([&] { unload(); returned = GetTickCount64(); });

    Sleep(holdMs);
    SetEvent(g_letRenderFinish);
    unloader.join();
    render.join();
    return returned - start;
}

// The unload does not return, and so the module is not freed, while the
// panel's renderFn is still running in it.
static bool g_forgetResult = false;

static void ForgetThisModule()
{
    g_forgetResult = Registry::ForgetModule(GetModuleHandleW(nullptr));
}

static void Test_UnloadWaitsForRender()
{
    OpenSlowPanel();
    const ULONGLONG took = UnloadDuringRender(ForgetThisModule, 300);
    CHECK(took >= 250);
    CHECK(g_forgetResult);   // safe to free the module
    CHECK(TestDoubles::loggedErrors == 0);
    CHECK(PanelCount() == 0);
}

// A renderFn that registers another panel while its module is being unloaded
// does not leave that panel behind.
static void Test_PanelRegisteredDuringUnloadIsForgotten()
{
    OpenSlowPanel();
    g_registerAfterHold = true;
    UnloadDuringRender([] { Registry::ForgetModule(GetModuleHandleW(nullptr)); }, 300);
    g_registerAfterHold = false;
    CHECK(PanelCount() == 0);
    Registry::ForgetModule(GetModuleHandleW(nullptr));
}

// The plugin's own UnregisterPanel waits too: it may free desc right after.
static void Test_UnregisterWaitsForRender()
{
    OpenSlowPanel();
    const ULONGLONG took = UnloadDuringRender([] { Registry::UnregisterPanel(g_slowPanel); }, 300);
    CHECK(took >= 250);
    CHECK(TestDoubles::loggedErrors == 0);
    CHECK(PanelCount() == 0);
}

// A panel-closed callback is plugin code called outside the lock as well. Plugin
// B's panel is closed on one thread, plugin A's callback holds it there, and A
// is unloaded from another.
static PanelHandle g_closingPanel;

static void ClosedSlow(PanelHandle)
{
    SetEvent(g_inRender);
    WaitForSingleObject(g_letRenderFinish, 10000);
}

static void Test_UnloadWaitsForClosedCallback()
{
    const PluginPanelDesc panelB = { "B", "Panel B", RenderB() };
    TestDoubles::loggedErrors = 0;

    Registry::RegisterOnPanelWindowClosed(ClosedSlow);
    g_closingPanel = Registry::RegisterPanel(&panelB);
    Registry::SetPanelOpen(g_closingPanel);

    const ULONGLONG took = UnloadDuringRender([] { Registry::ForgetModule(GetModuleHandleW(nullptr)); }, 300,
                                              [] { Registry::SetPanelClose(g_closingPanel); });
    CHECK(took >= 250);
    CHECK(TestDoubles::loggedErrors == 0);

    Registry::ForgetModule(ModuleB());
    CHECK(PanelCount() == 0);
}

// A renderFn that never returns cannot hang the unload: it gives up after the
// timeout, logs, and reports that the module is still in use, so the plugin
// manager keeps it mapped. The panel is gone all the same.
static void Test_WaitIsBounded()
{
    OpenSlowPanel();
    g_forgetResult = true;
    const DWORD timeout = UI::PluginCallTracker::kWaitTimeoutMs;
    const ULONGLONG took = UnloadDuringRender(ForgetThisModule, timeout + 700);
    CHECK(took >= timeout - 50);
    CHECK(took < timeout + 500);
    CHECK(!g_forgetResult);  // must not be freed
    CHECK(TestDoubles::loggedErrors == 1);
    CHECK(PanelCount() == 0);
}

// A renderFn may unregister its own panel. That call is on the render thread
// itself, so it must not wait for the render it is part of.
static void Test_RenderMayUnregisterItself()
{
    OpenSlowPanel();
    g_unregisterFromRender = true;

    const ULONGLONG start = GetTickCount64();
    std::thread render = StartRender();   // returns once UnregisterPanel has
    const ULONGLONG took = GetTickCount64() - start;
    SetEvent(g_letRenderFinish);
    render.join();
    g_unregisterFromRender = false;

    CHECK(took < 500);
    CHECK(TestDoubles::loggedErrors == 0);
    CHECK(PanelCount() == 0);
}

// The panels to draw are snapshotted at the start of the frame. One forgotten
// while an earlier panel's renderFn runs is not drawn: its entry is gone, and
// with it the descriptor the old code went on to read.
static void Test_ForgottenPanelIsNotDrawnLater()
{
    const PluginPanelDesc panelB = { "B", "Panel B", RenderB() };   // GetLastError: safe to call

    OpenSlowPanel();
    PanelHandle b = Registry::RegisterPanel(&panelB);
    Registry::SetPanelOpen(b);

    std::thread render = StartRender();
    const ULONGLONG start = GetTickCount64();
    Registry::ForgetModule(ModuleB());   // nothing of B's is running: returns at once
    const ULONGLONG took = GetTickCount64() - start;
    SetEvent(g_letRenderFinish);
    render.join();

    CHECK(took < 250);
    CHECK(TestDoubles::drawnTitles.size() == 1);
    CHECK(!TestDoubles::drawnTitles.empty() && TestDoubles::drawnTitles[0] == "Slow panel");

    Registry::ForgetModule(GetModuleHandleW(nullptr));
    CHECK(PanelCount() == 0);
}

// ---------------------------------------------------------------------------
// PluginGuard: a fault in a plugin callback switches off that plugin only.
// This executable is plugin "A" again; kernel32 is plugin "B".
// ---------------------------------------------------------------------------

static int g_guardReports = 0;
static PluginGuard::Report g_lastReport = {};
static void CountReport(const PluginGuard::Report& r) { ++g_guardReports; g_lastReport = r; }

static int g_okCallsA = 0;
static void FaultA(int) { *static_cast<volatile int*>(nullptr) = 1; }
static void OkA(int v) { g_okCallsA += v; }

static HMODULE ThisModule() { return GetModuleHandleW(nullptr); }
static HMODULE Kernel32()   { return GetModuleHandleW(L"kernel32.dll"); }

static void Test_GuardFaultSwitchesOffOnlyThatPlugin()
{
    PluginGuard::SetReporter(CountReport);
    g_guardReports = 0;
    g_okCallsA     = 0;
    PluginGuard::Track(ThisModule(), "A");
    PluginGuard::Track(Kernel32(), "B");

    CHECK(PluginGuard::Invoke(OkA, "tick callback", 1));
    CHECK(g_okCallsA == 1);

    // The fault is contained: Invoke returns false and the test goes on.
    CHECK(!PluginGuard::Invoke(FaultA, "tick callback", 0));
    CHECK(g_guardReports == 1);
    CHECK(g_lastReport.module == ThisModule());
    CHECK(g_lastReport.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(strcmp(g_lastReport.pluginName, "A") == 0);

    PluginGuard::Fault fault;
    CHECK(PluginGuard::GetFault(ThisModule(), &fault));
    CHECK(fault.code == EXCEPTION_ACCESS_VIOLATION);
    CHECK(fault.callback && strcmp(fault.callback, "tick callback") == 0);

    // Every other callback of A is skipped from now on, and not reported again.
    CHECK(PluginGuard::IsOff(reinterpret_cast<const void*>(OkA)));
    CHECK(!PluginGuard::Invoke(OkA, "tick callback", 1));
    CHECK(g_okCallsA == 1);
    CHECK(!PluginGuard::Invoke(FaultA, "tick callback", 0));
    CHECK(g_guardReports == 1);

    // B runs on.
    CHECK(!PluginGuard::IsOff(reinterpret_cast<const void*>(GetCurrentThreadId)));
    CHECK(!PluginGuard::GetFault(Kernel32(), nullptr));
    DWORD tid = 0;
    CHECK(PluginGuard::InvokeInto(&tid, GetCurrentThreadId, "tick callback"));
    CHECK(tid == GetCurrentThreadId());

    // Unloading (or reloading) A forgets the fault: it starts out running.
    PluginGuard::Untrack(ThisModule());
    CHECK(!PluginGuard::IsOff(reinterpret_cast<const void*>(OkA)));
    CHECK(!PluginGuard::GetFault(ThisModule(), nullptr));
    PluginGuard::Track(ThisModule(), "A");
    CHECK(PluginGuard::Invoke(OkA, "tick callback", 1));
    CHECK(g_okCallsA == 2);

    PluginGuard::Untrack(ThisModule());
    PluginGuard::Untrack(Kernel32());
}

// A fault in a callback that is in no plugin (the loader's own) is not caught.
static bool FaultOutsideAnyPluginPropagates()
{
    __try { PluginGuard::Invoke(FaultA, "tick callback", 0); return false; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

static void Test_GuardLeavesLoaderFaultsAlone()
{
    g_guardReports = 0;
    CHECK(FaultOutsideAnyPluginPropagates());
    CHECK(g_guardReports == 0);
    CHECK(!PluginGuard::GetFault(ThisModule(), nullptr));
}

// A fault raised outside the plugin, in code its callback called (here
// ntdll's strlen, standing in for the engine or the loader), is not contained:
// the frames between it and the guard may hold locks. It reaches the caller's
// handler, and the plugin is not switched off.
static size_t (__cdecl* g_ntdllStrlen)(const char*) = nullptr;
static void FaultBelowA(int) { volatile size_t n = g_ntdllStrlen(nullptr); (void)n; }

static bool FaultBelowPluginPropagates()
{
    __try { PluginGuard::Invoke(FaultBelowA, "tick callback", 0); return false; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

static void Test_GuardPassesOnFaultsOutsideThePlugin()
{
    g_ntdllStrlen = reinterpret_cast<size_t (__cdecl*)(const char*)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "strlen"));
    CHECK(g_ntdllStrlen != nullptr);
    if (!g_ntdllStrlen) return;

    PluginGuard::SetReporter(CountReport);
    g_guardReports = 0;
    TestDoubles::loggedErrors = 0;
    PluginGuard::Track(ThisModule(), "A");

    CHECK(FaultBelowPluginPropagates());
    CHECK(g_guardReports == 0);
    CHECK(!PluginGuard::GetFault(ThisModule(), nullptr));
    CHECK(TestDoubles::loggedErrors == 1);          // one line naming A and the callback
    CHECK(PluginGuard::Invoke(OkA, "tick callback", 0));

    PluginGuard::Untrack(ThisModule());
}

// C++ exceptions are not the guard's: they reach the caller's catch, and the
// plugin keeps running.
static void ThrowA(int) { throw 42; }

static void Test_GuardLetsCppExceptionsThrough()
{
    g_guardReports = 0;
    PluginGuard::Track(ThisModule(), "A");

    int caught = 0;
    try { PluginGuard::Invoke(ThrowA, "tick callback", 0); }
    catch (int v) { caught = v; }
    CHECK(caught == 42);
    CHECK(g_guardReports == 0);
    CHECK(!PluginGuard::GetFault(ThisModule(), nullptr));
    CHECK(PluginGuard::Invoke(OkA, "tick callback", 0));

    PluginGuard::Untrack(ThisModule());
}

// Two threads fault in the same plugin at once: both are contained, and the
// plugin is switched off and reported once.
static void Test_GuardTwoThreadsFaultAtOnce()
{
    g_guardReports = 0;
    PluginGuard::Track(ThisModule(), "A");

    std::atomic<int> ready{ 0 };
    bool ran[2] = { true, true };
    auto fault = [&](int i) {
        ++ready;
        while (ready.load() < 2) {}
        ran[i] = PluginGuard::Invoke(FaultA, "tick callback", 0);
    };
    std::thread t0(fault, 0), t1(fault, 1);
    t0.join();
    t1.join();

    CHECK(!ran[0] && !ran[1]);
    CHECK(g_guardReports == 1);
    CHECK(PluginGuard::GetFault(ThisModule(), nullptr));

    PluginGuard::Untrack(ThisModule());
}

// A call into memory that is no longer code (the callback of a DLL that is
// gone) is contained; a call into a live thunk that faults is not stale, and
// is no plugin's, so it goes on as before.
static bool CallThunkPropagates(void (*thunk)(int))
{
    __try { PluginGuard::Invoke(thunk, "tick callback", 0); return false; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

static void Test_GuardStaleCallbacks()
{
    g_guardReports = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);

    void* data = VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(data != nullptr);
    auto notCode = reinterpret_cast<void (*)(int)>(data);
    CHECK(!PluginGuard::Invoke(notCode, "tick callback", 0));   // not executable
    VirtualFree(data, 0, MEM_RELEASE);
    CHECK(!PluginGuard::Invoke(notCode, "tick callback", 0));   // not mapped at all
    CHECK(g_guardReports == 0);                                  // nothing to switch off

    // mov dword ptr [0], 1 ; ret
    static const unsigned char kFaultingThunk[] = { 0xC7, 0x04, 0x25, 0, 0, 0, 0, 1, 0, 0, 0, 0xC3 };
    void* code = VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(code != nullptr);
    memcpy(code, kFaultingThunk, sizeof(kFaultingThunk));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(kFaultingThunk));
    CHECK(CallThunkPropagates(reinterpret_cast<void (*)(int)>(code)));
    VirtualFree(code, 0, MEM_RELEASE);
    CHECK(g_guardReports == 0);
}

// A panel render that crashes halfway leaves ImGui as it was before the
// render, and the panel is not drawn again while its plugin is off.
static int g_crashRenders = 0;
static void CrashingRender(IModLoaderImGui*)
{
    ++g_crashRenders;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    ImGui::PushID("half-done");
    *static_cast<volatile int*>(nullptr) = 1;
}
static const PluginPanelDesc kCrashingPanel = { "Crash", "Crashing panel", CrashingRender };

static void Test_GuardRestoresImGuiAfterRenderFault()
{
    g_guardReports = 0;
    g_crashRenders = 0;
    PluginGuard::Track(ThisModule(), "A");
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;

    PanelHandle h = Registry::RegisterPanel(&kCrashingPanel);
    CHECK(h != nullptr);
    Registry::SetPanelOpen(h);

    ImGuiContext& g = *GImGui;
    const int styleVars = g.StyleVarStack.Size;
    const int ids       = g.CurrentWindow ? g.CurrentWindow->IDStack.Size : 0;

    DrawPanels();
    CHECK(g_crashRenders == 1);
    CHECK(g_guardReports == 1);
    CHECK(g.StyleVarStack.Size == styleVars);
    CHECK((g.CurrentWindow ? g.CurrentWindow->IDStack.Size : 0) == ids);

    DrawPanels();
    CHECK(g_crashRenders == 1);    // switched off: skipped

    Registry::ForgetModule(ThisModule());
    PluginGuard::Untrack(ThisModule());
}

// ---------------------------------------------------------------------------
// v75: title-bar callback and collapse
// ---------------------------------------------------------------------------

static int              g_renderCalls   = 0;
static int              g_titleBarCalls = 0;
static IModLoaderImGui* g_titleBarArg   = nullptr;
static PanelHandle      g_titleBarPanel = nullptr;
static bool             g_collapseFromTitleBar = false;
static int              g_imguiMarker   = 0;

static IModLoaderImGui* FakeImgui() { return reinterpret_cast<IModLoaderImGui*>(&g_imguiMarker); }

static void RenderCounted(IModLoaderImGui*) { ++g_renderCalls; }
static void TitleBarCounted(IModLoaderImGui* imgui)
{
    ++g_titleBarCalls;
    g_titleBarArg = imgui;
    if (g_collapseFromTitleBar)   // what a plugin's collapse button does
    {
        g_collapseFromTitleBar = false;
        Registry::SetPanelCollapsed(g_titleBarPanel, !Registry::IsPanelCollapsed(g_titleBarPanel));
    }
}

static const PluginPanelDesc kTitleBarPanel = { "T", "Title bar panel", RenderCounted, TitleBarCounted };

static void DrawTitleBarPanels()
{
    TestDoubles::drawnTitles.clear();
    TestDoubles::drawnFlags.clear();
    TestDoubles::drawnWithTitleBar.clear();
    g_renderCalls   = 0;
    g_titleBarCalls = 0;
    Registry::RenderPanelWindows(FakeImgui());
}

static void ForgetAllPanels()
{
    Registry::ForgetModule(ThisModule());
    Registry::ForgetModule(Kernel32());
}

// A plugin that declares v75 gets its title-bar callback run inside the window,
// with the imgui table it gave the panel.
static void Test_TitleBarCallbackRuns()
{
    TestDoubles::declaredInterface = 75;
    PanelHandle h = Registry::RegisterPanel(&kTitleBarPanel);
    CHECK(h != nullptr);
    Registry::SetPanelOpen(h);

    DrawTitleBarPanels();
    CHECK(TestDoubles::drawnWithTitleBar.size() == 1 && TestDoubles::drawnWithTitleBar[0]);
    CHECK(g_titleBarCalls == 1);
    CHECK(g_titleBarArg == FakeImgui());
    CHECK(g_renderCalls == 1);

    // A panel with no callback draws the same title bar as before.
    ForgetAllPanels();
    PanelHandle plain = Registry::RegisterPanel(&kPanelA);
    Registry::SetPanelOpen(plain);
    DrawTitleBarPanels();
    CHECK(TestDoubles::drawnWithTitleBar.size() == 1 && !TestDoubles::drawnWithTitleBar[0]);
    ForgetAllPanels();
}

// A descriptor from a plugin built before v75 is three pointers long. It is
// placed so the next byte is on a page that cannot be read: reading titleBarFn
// would fault, so a clean pass shows the loader stops at the end of the struct.
struct PanelDescBeforeV75
{
    const char* buttonLabel;
    const char* windowTitle;
    PluginImGuiRenderCallback renderFn;
};
static_assert(sizeof(PanelDescBeforeV75) == offsetof(PluginPanelDesc, titleBarFn), "the v70-v74 descriptor");

static void Test_OldSizedDescriptorIsNeverReadPastItsEnd()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    char* base = static_cast<char*>(VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(base != nullptr);
    DWORD previous = 0;
    VirtualProtect(base + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &previous);
    PanelDescBeforeV75* old = reinterpret_cast<PanelDescBeforeV75*>(base + info.dwPageSize - sizeof(PanelDescBeforeV75));
    *old = { "Old", "Old panel", RenderCounted };

    TestDoubles::declaredInterface = 74;
    PanelHandle h = Registry::RegisterPanel(reinterpret_cast<const PluginPanelDesc*>(old));
    CHECK(h != nullptr);
    Registry::SetPanelOpen(h);
    DrawTitleBarPanels();
    CHECK(TestDoubles::drawnTitles.size() == 1);
    CHECK(TestDoubles::drawnWithTitleBar.size() == 1 && !TestDoubles::drawnWithTitleBar[0]);
    CHECK(g_renderCalls == 1);
    CHECK(g_titleBarCalls == 0);

    // The new calls work on it too: it is only the descriptor that is short.
    Registry::SetPanelCollapsed(h, true);
    CHECK(Registry::IsPanelCollapsed(h));
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 0);

    ForgetAllPanels();
    VirtualFree(base, 0, MEM_RELEASE);
    TestDoubles::declaredInterface = 75;
}

// A full-size descriptor from a plugin that declares v74 or less has nothing
// in the new field that the loader may use, whatever the bytes are.
static void Test_TitleBarCallbackIgnoredBelowV75()
{
    TestDoubles::declaredInterface = 74;
    PanelHandle h = Registry::RegisterPanel(&kTitleBarPanel);
    Registry::SetPanelOpen(h);
    DrawTitleBarPanels();
    CHECK(TestDoubles::drawnWithTitleBar.size() == 1 && !TestDoubles::drawnWithTitleBar[0]);
    CHECK(g_titleBarCalls == 0);
    ForgetAllPanels();
    TestDoubles::declaredInterface = 75;

    // No plugin owns the module at all (reads as 0): same.
    TestDoubles::declaredInterface = 0;
    h = Registry::RegisterPanel(&kTitleBarPanel);
    Registry::SetPanelOpen(h);
    DrawTitleBarPanels();
    CHECK(TestDoubles::drawnWithTitleBar.size() == 1 && !TestDoubles::drawnWithTitleBar[0]);
    ForgetAllPanels();
    TestDoubles::declaredInterface = 75;
}

// A callback in another module than the panel's render could outlive an unload
// of the panel's module, so it is not kept.
static void Test_TitleBarCallbackInAnotherModuleIsDropped()
{
    const PluginPanelDesc foreign = { "F", "Foreign title bar", RenderCounted, RenderB() };
    PanelHandle h = Registry::RegisterPanel(&foreign);
    CHECK(h != nullptr);
    Registry::SetPanelOpen(h);
    DrawTitleBarPanels();
    CHECK(TestDoubles::drawnWithTitleBar.size() == 1 && !TestDoubles::drawnWithTitleBar[0]);
    ForgetAllPanels();
}

static void Test_CollapseStateAndRender()
{
    PanelHandle h = Registry::RegisterPanel(&kTitleBarPanel);
    Registry::SetPanelOpen(h);

    CHECK(!Registry::IsPanelCollapsed(h));
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 1);
    CHECK((TestDoubles::drawnFlags[0] & ImGuiWindowFlags_AlwaysAutoResize) == 0);

    // Collapsed: the title bar is drawn, the content is not.
    Registry::SetPanelCollapsed(h, true);
    CHECK(Registry::IsPanelCollapsed(h));
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 0);
    CHECK(g_titleBarCalls == 1);
    CHECK((TestDoubles::drawnFlags[0] & ImGuiWindowFlags_AlwaysAutoResize) != 0);
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 0 && g_titleBarCalls == 1);

    // Expanded again: the content is back and the window is no longer auto-sized.
    Registry::SetPanelCollapsed(h, false);
    CHECK(!Registry::IsPanelCollapsed(h));
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 1);
    CHECK((TestDoubles::drawnFlags[0] & ImGuiWindowFlags_AlwaysAutoResize) == 0);

    // The state is kept while the panel is closed, and applies when it opens.
    Registry::SetPanelClose(h);
    Registry::SetPanelCollapsed(h, true);
    Registry::SetPanelOpen(h);
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 0 && g_titleBarCalls == 1);

    ForgetAllPanels();
}

// The collapse button is drawn by the plugin, from inside the title bar: the
// call into the registry from there must not deadlock, and takes effect on the
// next frame.
static void Test_CollapseFromTheTitleBarCallback()
{
    g_titleBarPanel = Registry::RegisterPanel(&kTitleBarPanel);
    Registry::SetPanelOpen(g_titleBarPanel);

    g_collapseFromTitleBar = true;
    DrawTitleBarPanels();
    CHECK(Registry::IsPanelCollapsed(g_titleBarPanel));
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 0);

    g_collapseFromTitleBar = true;
    DrawTitleBarPanels();
    CHECK(!Registry::IsPanelCollapsed(g_titleBarPanel));
    DrawTitleBarPanels();
    CHECK(g_renderCalls == 1);

    ForgetAllPanels();
    g_titleBarPanel = nullptr;
}

static void Test_CollapseOfStaleOrNullHandle()
{
    PanelHandle h = Registry::RegisterPanel(&kTitleBarPanel);
    Registry::SetPanelCollapsed(h, true);
    ForgetAllPanels();   // the handle is stale now

    CHECK(!Registry::IsPanelCollapsed(h));
    Registry::SetPanelCollapsed(h, true);
    CHECK(!Registry::IsPanelCollapsed(h));
    CHECK(!Registry::IsPanelCollapsed(nullptr));
    Registry::SetPanelCollapsed(nullptr, true);
    CHECK(PanelCount() == 0);
}

// The helper every registry's ForgetModule uses: nothing of the freed module
// is left, everything else is.
struct GuardReg { int id; void (*cb)(int); };

static void Test_GuardForgetModuleDropsOnlyThatModule()
{
    std::vector<void (*)(int)> bare = { OkA, reinterpret_cast<void (*)(int)>(GetCurrentThreadId), FaultA };
    CHECK(PluginGuard::ForgetModule(bare, ThisModule()) == 2);
    CHECK(bare.size() == 1 && PluginGuard::ModuleOf(reinterpret_cast<const void*>(bare[0])) == Kernel32());

    std::vector<GuardReg> regs = { { 1, OkA }, { 2, reinterpret_cast<void (*)(int)>(GetLastError) }, { 3, FaultA } };
    CHECK(PluginGuard::ForgetModule(regs, ThisModule(), [](const GuardReg& r) { return r.cb; }) == 2);
    CHECK(regs.size() == 1 && regs[0].id == 2);
    for (const GuardReg& r : regs)
        CHECK(PluginGuard::ModuleOf(reinterpret_cast<const void*>(r.cb)) != ThisModule());

    CHECK(PluginGuard::ForgetModule(regs, nullptr, [](const GuardReg& r) { return r.cb; }) == 0);
    CHECK(regs.size() == 1);
}

int main()
{
    RunTest("PanelsPurgedByModule",              Test_PanelsPurgedByModule);
    RunTest("OpenPanelAndStaleHandle",           Test_OpenPanelAndStaleHandle);
    RunTest("ClosedCallbacksPurgedByModule",     Test_ClosedCallbacksPurgedByModule);
    RunTest("ForgetOfUnrelatedModuleIsHarmless", Test_ForgetOfUnrelatedModuleIsHarmless);
    RunTest("WidgetsPurgedByModule",             Test_WidgetsPurgedByModule);
    RunTest("UnloadPathsPurgePanels",            Test_UnloadPathsPurgePanels);
    RunTest("EveryForgetModuleIsCalledOnUnload", Test_EveryForgetModuleIsCalledOnUnload);

    // RenderPanelWindows sizes each window through Dear ImGui, which needs a
    // context; nothing is drawn.
    ImGui::CreateContext();
    // ...and a frame: a render is bracketed by ImGui's error-recovery state,
    // which reads the current window.
    ImGui::GetIO().DisplaySize  = ImVec2(1280.0f, 720.0f);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    TestDoubles::drawWindows = true;
    g_inRender         = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_letRenderFinish  = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    RunTest("UnloadWaitsForRender",              Test_UnloadWaitsForRender);
    RunTest("PanelRegisteredDuringUnloadIsForgotten", Test_PanelRegisteredDuringUnloadIsForgotten);
    RunTest("UnregisterWaitsForRender",          Test_UnregisterWaitsForRender);
    RunTest("UnloadWaitsForClosedCallback",      Test_UnloadWaitsForClosedCallback);
    RunTest("WaitIsBounded",                     Test_WaitIsBounded);
    RunTest("RenderMayUnregisterItself",         Test_RenderMayUnregisterItself);
    RunTest("ForgottenPanelIsNotDrawnLater",     Test_ForgottenPanelIsNotDrawnLater);

    RunTest("GuardFaultSwitchesOffOnlyThatPlugin",  Test_GuardFaultSwitchesOffOnlyThatPlugin);
    RunTest("GuardLeavesLoaderFaultsAlone",         Test_GuardLeavesLoaderFaultsAlone);
    RunTest("GuardForgetModuleDropsOnlyThatModule", Test_GuardForgetModuleDropsOnlyThatModule);
    RunTest("GuardPassesOnFaultsOutsideThePlugin",  Test_GuardPassesOnFaultsOutsideThePlugin);
    RunTest("GuardLetsCppExceptionsThrough",        Test_GuardLetsCppExceptionsThrough);
    RunTest("GuardTwoThreadsFaultAtOnce",           Test_GuardTwoThreadsFaultAtOnce);
    RunTest("GuardStaleCallbacks",                  Test_GuardStaleCallbacks);
    RunTest("GuardRestoresImGuiAfterRenderFault",   Test_GuardRestoresImGuiAfterRenderFault);

    RunTest("TitleBarCallbackRuns",                 Test_TitleBarCallbackRuns);
    RunTest("OldSizedDescriptorIsNeverReadPastItsEnd", Test_OldSizedDescriptorIsNeverReadPastItsEnd);
    RunTest("TitleBarCallbackIgnoredBelowV75",      Test_TitleBarCallbackIgnoredBelowV75);
    RunTest("TitleBarCallbackInAnotherModuleIsDropped", Test_TitleBarCallbackInAnotherModuleIsDropped);
    RunTest("CollapseStateAndRender",               Test_CollapseStateAndRender);
    RunTest("CollapseFromTheTitleBarCallback",      Test_CollapseFromTheTitleBarCallback);
    RunTest("CollapseOfStaleOrNullHandle",          Test_CollapseOfStaleOrNullHandle);

    CloseHandle(g_letRenderFinish);
    CloseHandle(g_inRender);
    ImGui::DestroyContext();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
