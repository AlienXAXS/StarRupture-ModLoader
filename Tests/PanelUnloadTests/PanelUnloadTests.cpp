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
#include "imgui/imgui.h"
#include "test_doubles.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

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
static std::string FunctionBody(const std::string& src, const char* signature)
{
    const size_t start = src.find(signature);
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
        "bool UnloadPlugin(int index)",
        "bool ReloadPlugin(int index)",
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

        for (const char* registry : { "PluginPanelRegistry", "PluginWidgetRegistry" })
        {
            const size_t forget = body.find(std::string(registry) + "::ForgetModule(" + module + ")");
            CHECK(forget != std::string::npos);
            CHECK(forget < free);
        }

        // The overlay may still be in the module when the wait gives up; the
        // path must then stop before FreeLibrary.
        const size_t refuse = body.find("if (!WaitOutPluginRenders(" + module + ",");
        CHECK(refuse != std::string::npos);
        CHECK(refuse < free);
    }

    // ...and that wait must come from both registries, with its answer used.
    const std::string wait = FunctionBody(src, "static bool WaitOutPluginRenders(");
    CHECK(wait.find("= UI::PluginPanelRegistry::ForgetModule(module)") != std::string::npos);
    CHECK(wait.find("= UI::PluginWidgetRegistry::ForgetModule(module)") != std::string::npos);
    CHECK(wait.find("return false;") != std::string::npos);
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

int main()
{
    RunTest("PanelsPurgedByModule",              Test_PanelsPurgedByModule);
    RunTest("OpenPanelAndStaleHandle",           Test_OpenPanelAndStaleHandle);
    RunTest("ClosedCallbacksPurgedByModule",     Test_ClosedCallbacksPurgedByModule);
    RunTest("ForgetOfUnrelatedModuleIsHarmless", Test_ForgetOfUnrelatedModuleIsHarmless);
    RunTest("WidgetsPurgedByModule",             Test_WidgetsPurgedByModule);
    RunTest("UnloadPathsPurgePanels",            Test_UnloadPathsPurgePanels);

    // RenderPanelWindows sizes each window through Dear ImGui, which needs a
    // context; nothing is drawn.
    ImGui::CreateContext();
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

    CloseHandle(g_letRenderFinish);
    CloseHandle(g_inRender);
    ImGui::DestroyContext();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
