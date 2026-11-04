// IsKeyHeld and IsGameThread test: the two v72 queries a plugin's tick polls.
//
// A console program: no game, no overlay. keybind_registry.cpp and
// game_thread_dispatch.cpp are the loader's own source files; the keyboard,
// the foreground window and the text fields are test doubles (pch.h,
// test_doubles.cpp), so the tests hold keys, move focus and type without
// touching the real desktop. Key messages go straight to the registry's
// ProcessWindowMessage, as the overlay's window procedure hands them over.
#include "hooks/input/keybind_registry.h"
#include "utils/game_thread_dispatch.h"
#include "test_doubles.h"
#include "core/ui_state.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
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

// Every allocation in the process, so a test can see that IsKeyHeld makes none.
static std::atomic<long> g_allocations{ 0 };

void* operator new(size_t size)
{
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using Hooks::Input::IsKeyHeld;
using Hooks::Input::ProcessWindowMessage;
using TestDoubles::SetKeyDown;

// Two windows that are never created; only their identity is compared.
static const HWND kGame  = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1000));
static const HWND kOther = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x2000));

// The game window exists and has focus, and nothing is held.
static void Reset()
{
    TestDoubles::ReleaseAllKeys();
    TestDoubles::gameWindow = kGame;
    TestDoubles::foreground = kGame;
    TestDoubles::buttonsSwapped = false;
    TestDoubles::gameTextFocused = false;
    TestDoubles::imguiTextActive = false;
}

// A key going down or up in the game window: the keyboard state, then the
// message (a first press, not an auto-repeat).
static void Press(int vk)
{
    SetKeyDown(vk, true);
    ProcessWindowMessage(WM_KEYDOWN, vk, 0);
}

static void Release(int vk)
{
    SetKeyDown(vk, false);
    ProcessWindowMessage(WM_KEYUP, vk, 0xC0000000);
}

// ---------------------------------------------------------------------------
// IsKeyHeld
// ---------------------------------------------------------------------------

static void SingleKey()
{
    Reset();
    CHECK(!IsKeyHeld("F5"));
    SetKeyDown(VK_F5, true);
    CHECK(IsKeyHeld("F5"));
    CHECK(IsKeyHeld("f5"));                // names are case-insensitive, as the registry's are
    CHECK(!IsKeyHeld("F6"));
    SetKeyDown('K', true);
    CHECK(IsKeyHeld("K"));
    SetKeyDown(VK_F5, false);
    CHECK(!IsKeyHeld("F5"));
    CHECK(IsKeyHeld("K"));
}

static void ModifierChords()
{
    Reset();
    SetKeyDown('K', true);
    CHECK(!IsKeyHeld("Shift+K"));          // K alone is not Shift+K
    SetKeyDown(VK_LSHIFT, true);
    CHECK(IsKeyHeld("Shift+K"));
    CHECK(IsKeyHeld("shift + k"));         // the registry's parser trims and ignores case
    SetKeyDown(VK_LSHIFT, false);
    SetKeyDown(VK_RSHIFT, true);
    CHECK(IsKeyHeld("Shift+K"));           // either side counts, as SampleCurrentModifiers says

    Reset();
    SetKeyDown(VK_F5, true);
    SetKeyDown(VK_LCONTROL, true);
    CHECK(!IsKeyHeld("Ctrl+Shift+F5"));    // every modifier named must be held
    SetKeyDown(VK_RSHIFT, true);
    CHECK(IsKeyHeld("Ctrl+Shift+F5"));
    CHECK(IsKeyHeld("Shift+Control+F5"));  // token order and "Control", as the registry reads them
    SetKeyDown(VK_F5, false);
    CHECK(!IsKeyHeld("Ctrl+Shift+F5"));    // modifiers alone are not the chord

    Reset();
    SetKeyDown(VK_F7, true);
    SetKeyDown(VK_LMENU, true);
    CHECK(IsKeyHeld("Alt+F7"));
}

// DispatchCombo's rule for a named entry: modifiers must match exactly, and a
// name without modifiers ignores them.
static void ExtraModifierHeld()
{
    Reset();
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    SetKeyDown(VK_LCONTROL, true);
    CHECK(!IsKeyHeld("Shift+K"));          // Ctrl is held as well: not exactly Shift
    CHECK(IsKeyHeld("Ctrl+Shift+K"));
    CHECK(IsKeyHeld("K"));                 // no modifiers named: held whatever else is
    SetKeyDown(VK_LCONTROL, false);
    CHECK(IsKeyHeld("Shift+K"));
}

static void ModifierAsKey()
{
    Reset();
    CHECK(!IsKeyHeld("LeftShift"));
    SetKeyDown(VK_LSHIFT, true);
    CHECK(IsKeyHeld("LeftShift"));
    CHECK(!IsKeyHeld("RightShift"));
    CHECK(!IsKeyHeld("Shift"));            // a modifier token alone names no key
}

static void MouseButtons()
{
    Reset();
    CHECK(!IsKeyHeld("LeftMouseButton"));
    SetKeyDown(VK_LBUTTON, true);
    CHECK(IsKeyHeld("LeftMouseButton"));
    CHECK(!IsKeyHeld("RightMouseButton"));
    SetKeyDown(VK_XBUTTON2, true);
    CHECK(IsKeyHeld("ThumbMouseButton2"));
    CHECK(!IsKeyHeld("ThumbMouseButton"));
    SetKeyDown(VK_LCONTROL, true);
    CHECK(IsKeyHeld("Ctrl+LeftMouseButton"));
}

// GetAsyncKeyState reports the physical buttons; the callbacks and the game
// see the logical ones. With the buttons swapped, the physical left button is
// the logical right one.
static void SwappedMouseButtons()
{
    Reset();
    TestDoubles::buttonsSwapped = true;
    SetKeyDown(VK_LBUTTON, true);          // physical left
    CHECK(IsKeyHeld("RightMouseButton"));
    CHECK(!IsKeyHeld("LeftMouseButton"));
    SetKeyDown(VK_LBUTTON, false);
    SetKeyDown(VK_RBUTTON, true);          // physical right
    CHECK(IsKeyHeld("LeftMouseButton"));
    CHECK(!IsKeyHeld("RightMouseButton"));
    SetKeyDown(VK_MBUTTON, true);          // only left and right swap
    CHECK(IsKeyHeld("MiddleMouseButton"));
}

// The key below Escape, whose VK depends on the keyboard layout.
static void TildeFollowsLayout()
{
    Reset();
    const int vk = Hooks::Input::ModKeyToVK(EModKey::Tilde);
    CHECK(vk != 0);
    SetKeyDown(vk, true);
    CHECK(IsKeyHeld("Tilde"));
}

static void UnknownNames()
{
    Reset();
    for (int vk = 1; vk < 256; ++vk)
        SetKeyDown(vk, true);              // everything is down: only the name can say no
    CHECK(!IsKeyHeld(nullptr));
    CHECK(!IsKeyHeld(""));
    CHECK(!IsKeyHeld("NotAKey"));
    CHECK(!IsKeyHeld("Ctrl+NotAKey"));
    CHECK(!IsKeyHeld("+"));
    CHECK(!IsKeyHeld("Ctrl+Shift"));

    // RegisterKeybindByName's parser, kept as it is: a token it does not know
    // is skipped, and a later key replaces an earlier one.
    Reset();
    SetKeyDown('K', true);
    CHECK(IsKeyHeld("Ctlr+K"));            // a misspelt modifier reads as K
    CHECK(IsKeyHeld("W+K"));               // K replaces W; W is not held
    CHECK(!IsKeyHeld("K+W"));              // W replaces K
}

static void NoFocus()
{
    Reset();
    SetKeyDown(VK_F5, true);
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    TestDoubles::foreground = kOther;      // the player alt-tabbed out
    CHECK(!IsKeyHeld("F5"));
    CHECK(!IsKeyHeld("Shift+K"));
    TestDoubles::foreground = nullptr;     // nothing has focus (a desktop switch)
    CHECK(!IsKeyHeld("F5"));
    TestDoubles::foreground = kGame;
    CHECK(IsKeyHeld("F5"));
}

static void NullWindow()
{
    Reset();
    SetKeyDown(VK_F5, true);
    TestDoubles::gameWindow = nullptr;     // before the overlay's first Present
    CHECK(!IsKeyHeld("F5"));
    TestDoubles::foreground = nullptr;     // null must not match a null foreground
    CHECK(!IsKeyHeld("F5"));
}

static void AllocatesNothing()
{
    Reset();
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    const long before = g_allocations.load();
    bool any = false;
    for (int i = 0; i < 1000; ++i)
        any |= IsKeyHeld("Shift+K") | IsKeyHeld("Ctrl+Alt+F12") | IsKeyHeld("NotAKey");
    CHECK(any);
    CHECK(g_allocations.load() == before);
}

// The keybind callbacks' typing rule: a key pressed while a text field has
// focus is the player typing, so it does not read as held until released.
static void PressedWhileTyping()
{
    Reset();
    CHECK(!IsKeyHeld("P"));                // the first call: the loader watches P from here
    TestDoubles::gameTextFocused = true;   // chat is open
    Press('P');
    CHECK(!IsKeyHeld("P"));
    TestDoubles::gameTextFocused = false;  // chat closes with P still down
    CHECK(!IsKeyHeld("P"));                // still the press that went into chat
    Release('P');
    CHECK(!IsKeyHeld("P"));
    Press('P');
    CHECK(IsKeyHeld("P"));                 // pressed again outside a text field
    Release('P');

    Reset();
    TestDoubles::imguiTextActive = true;   // an input box in an overlay panel
    Press('P');
    CHECK(!IsKeyHeld("P"));
    Release('P');

    Reset();
    CHECK(!IsKeyHeld("Shift+L"));
    SetKeyDown(VK_LSHIFT, true);
    TestDoubles::gameTextFocused = true;
    Press('L');                            // a capital L typed into chat
    CHECK(!IsKeyHeld("Shift+L"));
    CHECK(!IsKeyHeld("L"));
    Release('L');
}

static void HeldBeforeTyping()
{
    Reset();
    CHECK(!IsKeyHeld("W"));
    Press('W');
    TestDoubles::gameTextFocused = true;   // the player clicks into chat, W still down
    CHECK(IsKeyHeld("W"));                 // its press reached the keybinds: still held
    ProcessWindowMessage(WM_KEYDOWN, 'W', 0x40000001);   // Windows repeats a held key
    CHECK(IsKeyHeld("W"));                 // a repeat is not a new press
    Release('W');
    CHECK(!IsKeyHeld("W"));

    Reset();
    CHECK(!IsKeyHeld("P"));
    TestDoubles::gameTextFocused = true;
    Press('P');
    TestDoubles::gameTextFocused = false;
    ProcessWindowMessage(WM_KEYDOWN, 'P', 0x40000001);
    CHECK(!IsKeyHeld("P"));                // still the press that went into chat
    Release('P');
}

// The rebind modal swallows key messages; a key-up it swallows still ends
// the press the typing gate held back.
static void ReleasedDuringCapture()
{
    Reset();
    CHECK(!IsKeyHeld("K"));
    TestDoubles::gameTextFocused = true;
    Press('K');
    CHECK(!IsKeyHeld("K"));
    TestDoubles::gameTextFocused = false;
    Hooks::Input::NoteKeybindCaptureFrame();
    Release('K');
    Press('K');                            // the press the modal reads, no gate
    CHECK(IsKeyHeld("K"));
    Release('K');
    Hooks::Input::EndKeybindCapture();
}

static void TypingExemptions()
{
    Reset();
    TestDoubles::gameTextFocused = true;
    CHECK(!IsKeyHeld("Escape"));
    Press(VK_ESCAPE);
    CHECK(IsKeyHeld("Escape"));            // Escape still closes a window from a text field
    Release(VK_ESCAPE);

    Hooks::Input::SetTypingExempt(EModKey::F8, true);
    CHECK(!IsKeyHeld("F8"));
    Press(VK_F8);
    CHECK(IsKeyHeld("F8"));
    Release(VK_F8);
    Hooks::Input::SetTypingExempt(EModKey::F8, false);
    Press(VK_F8);
    CHECK(!IsKeyHeld("F8"));
    Release(VK_F8);

    CHECK(!IsKeyHeld("MiddleMouseButton"));
    SetKeyDown(VK_MBUTTON, true);
    ProcessWindowMessage(WM_MBUTTONDOWN, MK_MBUTTON, 0);
    CHECK(IsKeyHeld("MiddleMouseButton")); // mouse buttons are never typing
    SetKeyDown(VK_MBUTTON, false);
    ProcessWindowMessage(WM_MBUTTONUP, 0, 0);
}

static std::atomic<int> g_jPressed{ 0 };
static void OnJ(EModKey, EModKeyEvent) { ++g_jPressed; }

// A key with a keybind: the poll gives the callback's answer, its first call
// included, and the callback still skips the press as before.
static void KeybindAndPollAgree()
{
    Reset();
    Hooks::Input::RegisterKeybind(EModKey::J, EModKeyEvent::Pressed, OnJ);
    TestDoubles::gameTextFocused = true;
    Press('J');
    CHECK(g_jPressed.load() == 0);
    CHECK(!IsKeyHeld("J"));
    Release('J');
    TestDoubles::gameTextFocused = false;
    Press('J');
    CHECK(g_jPressed.load() == 1);
    CHECK(IsKeyHeld("J"));
    Release('J');
    Hooks::Input::UnregisterKeybind(EModKey::J, EModKeyEvent::Pressed, OnJ);
}

// No keybind and no poll on a key: its press skips the game's text-focus
// scan, as it always has, so the loader has nothing on it for the first poll.
static void UnwatchedKeySkipsTheScan()
{
    Reset();
    TestDoubles::gameTextFocused = true;
    const long before = TestDoubles::gameTextChecks.load();
    Press('Q');
    CHECK(TestDoubles::gameTextChecks.load() == before);
    CHECK(IsKeyHeld("Q"));                 // pressed before the loader watched Q
    Release('Q');
    Press('Q');
    CHECK(TestDoubles::gameTextChecks.load() == before + 1);
    CHECK(!IsKeyHeld("Q"));
    Release('Q');
}

// Called from several threads at once while another moves the focus: every
// answer is one a single-threaded call could give, and nothing faults.
static void AnyThread()
{
    Reset();
    SetKeyDown('K', true);
    SetKeyDown(VK_LSHIFT, true);
    std::atomic<bool> stop{ false };
    std::atomic<long> held{ 0 }, notHeld{ 0 }, wrong{ 0 };
    std::thread focus([&] {
        for (int i = 0; i < 20000; ++i)
            TestDoubles::foreground = (i & 1) ? kOther : kGame;
        TestDoubles::foreground = kGame;
        stop = true;
    });
    std::thread readers[4];
    for (auto& t : readers)
        t = std::thread([&] {
            while (!stop.load())
            {
                if (IsKeyHeld("Shift+K")) ++held; else ++notHeld;
                if (IsKeyHeld("Ctrl+K")) ++wrong;      // Ctrl is never held
            }
        });
    focus.join();
    for (auto& t : readers)
        t.join();
    CHECK(wrong.load() == 0);
    CHECK(held.load() + notHeld.load() > 0);
    CHECK(IsKeyHeld("Shift+K"));                       // settled back on the game window
}

// ---------------------------------------------------------------------------
// IsGameThread (GameThreadDispatch: the thread that calls Drain each tick)
// ---------------------------------------------------------------------------

// Must run first: it checks the state before any tick.
static void GameThreadBeforeFirstTick()
{
    CHECK(!GameThreadDispatch::HasTicked());
    CHECK(!GameThreadDispatch::IsGameThread());       // the thread that will tick included
    bool other = true;
    std::thread([&] { other = GameThreadDispatch::IsGameThread(); }).join();
    CHECK(!other);
}

static void GameThreadAfterTick()
{
    // A thread that stands in for the game thread: it ticks (Drain) and stays
    // alive, so its id cannot be reused by another thread mid-test.
    std::atomic<int> phase{ 0 };
    bool onTickThread = false, inTask = false;
    std::thread game([&] {
        GameThreadDispatch::PostVoid([&] { inTask = GameThreadDispatch::IsGameThread(); });
        GameThreadDispatch::Drain();
        onTickThread = GameThreadDispatch::IsGameThread();
        phase = 1;
        while (phase.load() != 2)
            Sleep(1);
    });
    while (phase.load() != 1)
        Sleep(1);

    CHECK(GameThreadDispatch::HasTicked());
    CHECK(onTickThread);
    CHECK(inTask);                                     // a posted task runs on the game thread
    CHECK(!GameThreadDispatch::IsGameThread());       // this thread did not tick
    bool worker = true;
    std::thread([&] { worker = GameThreadDispatch::IsGameThread(); }).join();
    CHECK(!worker);

    phase = 2;
    game.join();

    // Whichever thread ticks last is the game thread.
    GameThreadDispatch::Drain();
    CHECK(GameThreadDispatch::IsGameThread());
}

// A plugin being unloaded: ForgetModule drops every keybind whose callback is
// in its module, from all three lists, and leaves the rest.
static std::atomic<int> g_kPressed{ 0 };
static void OnK(EModKey, EModKeyEvent) { ++g_kPressed; }
static void OnKCombo(EModKey, EModKeyModifiers, EModKeyEvent) { ++g_kPressed; }

static void ForgetModuleDropsItsKeybinds()
{
    Reset();
    Hooks::Input::RegisterKeybind(EModKey::K, EModKeyEvent::Pressed, OnK);
    Hooks::Input::RegisterKeybindByName("Ctrl+K", EModKeyEvent::Pressed, OnK);
    Hooks::Input::RegisterKeybindCombo(EModKey::K, EModKeyMod_None, EModKeyEvent::Pressed, OnKCombo);
    Press('K');
    Release('K');
    CHECK(g_kPressed.load() >= 1);

    // Another module's ForgetModule leaves them.
    Hooks::Input::ForgetModule(GetModuleHandleW(L"kernel32.dll"));
    const int before = g_kPressed.load();
    Press('K');
    Release('K');
    CHECK(g_kPressed.load() > before);

    Hooks::Input::ForgetModule(GetModuleHandleW(nullptr));
    const int after = g_kPressed.load();
    Press('K');
    Release('K');
    CHECK(g_kPressed.load() == after);
    SetKeyDown(VK_CONTROL, true);
    Press('K');
    Release('K');
    SetKeyDown(VK_CONTROL, false);
    CHECK(g_kPressed.load() == after);
}

// ForgetModule on the real queue: an unloading plugin's posted tasks never
// run, whether still queued or later in the batch Drain is running (RELOAD and
// UNLOAD run as a task, so the plugin's own tasks can sit right behind them).
// This executable is plugin A, kernel32 plugin B.
static void ForgetModuleDropsItsGameThreadTasks()
{
    const HMODULE a = GetModuleHandleW(nullptr);
    const HMODULE b = GetModuleHandleW(L"kernel32.dll");
    int ranA = 0, ranB = 0, ranLoader = 0;

    // Still queued.
    GameThreadDispatch::PostVoid([&] { ++ranA; }, a);
    GameThreadDispatch::PostVoid([&] { ++ranB; }, b);
    GameThreadDispatch::PostVoid([&] { ++ranLoader; });
    GameThreadDispatch::ForgetModule(a);
    GameThreadDispatch::ForgetModule(nullptr);   // the loader's own tasks are never dropped
    GameThreadDispatch::Drain();
    CHECK(ranA == 0);
    CHECK(ranB == 1);
    CHECK(ranLoader == 1);

    // Behind the unload, in the same batch.
    ranA = ranB = ranLoader = 0;
    GameThreadDispatch::PostVoid([&] { ++ranA; }, a);
    GameThreadDispatch::PostVoid([&] { GameThreadDispatch::ForgetModule(a); ++ranLoader; });
    GameThreadDispatch::PostVoid([&] { ++ranA; }, a);
    GameThreadDispatch::PostVoid([&] { ++ranB; }, b);
    GameThreadDispatch::PostVoid([&] { ++ranA; }, a);
    GameThreadDispatch::Drain();
    CHECK(ranA == 1);          // only the one ahead of the unload
    CHECK(ranB == 1);
    CHECK(ranLoader == 1);

    // Nothing is left over for the next tick, and A posts again once reloaded.
    GameThreadDispatch::Drain();
    CHECK(ranA == 1 && ranB == 1);
    GameThreadDispatch::PostVoid([&] { ++ranA; }, a);
    GameThreadDispatch::Drain();
    CHECK(ranA == 2);
}

// ---------------------------------------------------------------------------
// UI state (v74): the registry behind GetUIState and RegisterOnUIStateChanged.
// ui_state.cpp is the loader's own file; Publish stands in for the tick.
// ---------------------------------------------------------------------------

struct UICall { uint64_t oldState, newState; int tag; };
static std::vector<UICall> g_uiCalls;

static void OnUI(uint64_t oldState, uint64_t newState, void* userData)
{
    g_uiCalls.push_back({ oldState, newState, static_cast<int>(reinterpret_cast<intptr_t>(userData)) });
}

static void* Tag(int n) { return reinterpret_cast<void*>(static_cast<intptr_t>(n)); }

// Unregisters the tags the UI tests use and clears the state.
static void ResetUI()
{
    for (int n = 0; n < 4; ++n)
        UIState::Unregister(&OnUI, Tag(n));
    UIState::Publish(0);
    g_uiCalls.clear();
}

static void UIStateMaskAndOncePerChange()
{
    ResetUI();
    UIState::Register(PLUGIN_UI_MAP, &OnUI, Tag(1));

    // A bit outside the mask changes: not called.
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY | PLUGIN_UI_CURSOR_UP);
    CHECK(g_uiCalls.empty());

    // The map opens: called once, with the whole state either side.
    const uint64_t mapOpen = PLUGIN_UI_IN_GAMEPLAY | PLUGIN_UI_CURSOR_UP | PLUGIN_UI_ANY_MENU | PLUGIN_UI_MAP;
    UIState::Publish(mapOpen);
    UIState::Publish(mapOpen);
    UIState::Publish(mapOpen);
    CHECK(g_uiCalls.size() == 1);
    CHECK(g_uiCalls[0].oldState == (PLUGIN_UI_IN_GAMEPLAY | PLUGIN_UI_CURSOR_UP));
    CHECK(g_uiCalls[0].newState == mapOpen);
    // A query reports only the bits kept all the time.
    CHECK(UIState::Get() == (PLUGIN_UI_IN_GAMEPLAY | PLUGIN_UI_CURSOR_UP));

    // It closes: called once more.
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);
    CHECK(g_uiCalls.size() == 2);
    CHECK(g_uiCalls[1].oldState == mapOpen);
    ResetUI();
}

static void UIStateFirstComparisonIsAgainstZero()
{
    ResetUI();
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);

    // Registered while already in gameplay: the next tick reports it.
    UIState::Register(PLUGIN_UI_IN_GAMEPLAY, &OnUI, Tag(1));
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);
    CHECK(g_uiCalls.size() == 1);
    CHECK(g_uiCalls[0].oldState == 0);
    CHECK(g_uiCalls[0].newState == PLUGIN_UI_IN_GAMEPLAY);

    // A bit the mask leaves out is never reported, even the first time.
    UIState::Register(PLUGIN_UI_PAUSE_MENU, &OnUI, Tag(2));
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);
    CHECK(g_uiCalls.size() == 1);
    ResetUI();
}

static void UIStateDuplicateIgnored()
{
    ResetUI();
    UIState::Register(~0ull, &OnUI, Tag(1));
    UIState::Register(~0ull, &OnUI, Tag(1));
    UIState::Publish(PLUGIN_UI_PAUSE_MENU);
    CHECK(g_uiCalls.size() == 1);

    // The same callback with other userData is its own registration.
    UIState::Register(~0ull, &OnUI, Tag(2));
    UIState::Publish(PLUGIN_UI_CURSOR_UP);
    CHECK(g_uiCalls.size() == 3);
    ResetUI();
}

static void OnUIUnregistersBoth(uint64_t oldState, uint64_t newState, void* userData)
{
    OnUI(oldState, newState, userData);
    UIState::Unregister(&OnUIUnregistersBoth, Tag(1));
    UIState::Unregister(&OnUI, Tag(2));
}

static void UIStateUnregisterInsideCallback()
{
    ResetUI();
    UIState::Register(~0ull, &OnUIUnregistersBoth, Tag(1));
    UIState::Register(~0ull, &OnUI, Tag(2));

    // The first callback removes itself and the second: the second is not
    // called this tick, and neither is called again.
    UIState::Publish(PLUGIN_UI_CURSOR_UP);
    CHECK(g_uiCalls.size() == 1);
    CHECK(g_uiCalls[0].tag == 1);
    UIState::Publish(0);
    CHECK(g_uiCalls.size() == 1);
    ResetUI();
}

static void UIStateForgetModule()
{
    ResetUI();
    UIState::Register(~0ull, &OnUI, Tag(1));

    // Another module's unload leaves it.
    UIState::ForgetModule(GetModuleHandleW(L"kernel32.dll"));
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);
    CHECK(g_uiCalls.size() == 1);

    // Its own module's unload drops it.
    UIState::ForgetModule(GetModuleHandleW(nullptr));
    UIState::Publish(0);
    CHECK(g_uiCalls.size() == 1);
    ResetUI();
}

static void UIStateMenuReadDemand()
{
    ResetUI();
    CHECK(!UIState::MenuReadWanted());

    // A query does not start it, and never sees a menu bit.
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY | PLUGIN_UI_ANY_MENU | PLUGIN_UI_MAP);
    CHECK(UIState::Get() == PLUGIN_UI_IN_GAMEPLAY);
    CHECK(!UIState::MenuReadWanted());

    // A registration keeps it running for as long as it lasts.
    UIState::Register(PLUGIN_UI_MAP, &OnUI, Tag(1));
    CHECK(UIState::MenuReadWanted());
    UIState::Unregister(&OnUI, Tag(1));
    CHECK(!UIState::MenuReadWanted());

    // One for the cheap bits only does not start it.
    UIState::Register(PLUGIN_UI_CURSOR_UP | PLUGIN_UI_PAUSE_MENU, &OnUI, Tag(2));
    CHECK(!UIState::MenuReadWanted());
    ResetUI();
}

static void UIStateMenuBits()
{
    using UIState::MenuBits;
    using UIState::MenuKind;
    const uint64_t any = PLUGIN_UI_ANY_MENU;

    // Nothing on the layer: no bit.
    CHECK(MenuBits(0, nullptr, 0) == 0);

    // A screen on the layer sets ANY_MENU and its own bit. The widget list
    // keeps a closing screen until its transition has ended, so the same
    // list gives the same bits for the whole close animation.
    const MenuKind map[] = { MenuKind::Map };
    CHECK(MenuBits(1, map, 1) == (any | PLUGIN_UI_MAP));
    const MenuKind inventory[] = { MenuKind::Inventory };
    CHECK(MenuBits(1, inventory, 1) == (any | PLUGIN_UI_INVENTORY));
    const MenuKind build[] = { MenuKind::BuildMenu };
    CHECK(MenuBits(1, build, 1) == (any | PLUGIN_UI_BUILD_MENU));
    const MenuKind station[] = { MenuKind::StationMenu };
    CHECK(MenuBits(1, station, 1) == (any | PLUGIN_UI_STATION_MENU));

    // A screen without a bit of its own is ANY_MENU alone.
    const MenuKind other[] = { MenuKind::Other };
    CHECK(MenuBits(1, other, 1) == any);

    // A map closing under a pause menu: both stay on the layer, so the map
    // bit stays set until the map is gone.
    const MenuKind both[] = { MenuKind::Map, MenuKind::Other };
    CHECK(MenuBits(2, both, 2) == (any | PLUGIN_UI_MAP));
    const MenuKind gone[] = { MenuKind::Other };
    CHECK(MenuBits(1, gone, 1) == any);

    // A list longer than the kinds given is still not empty.
    CHECK(MenuBits(20, other, 1) == any);

    // Every bit it can set is one the menu read is gated on.
    CHECK((MenuBits(2, both, 2) & ~UIState::kMenuBits) == 0);

    // The state a callback sees: the map opens, closes under a pause menu,
    // and is gone. Only the changes of a bit in the mask are reported.
    ResetUI();
    UIState::Register(PLUGIN_UI_MAP, &OnUI, Tag(1));
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);
    CHECK(g_uiCalls.empty());
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY | MenuBits(1, map, 1));
    CHECK(g_uiCalls.size() == 1);
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY | MenuBits(2, both, 2));
    CHECK(g_uiCalls.size() == 1);
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY | MenuBits(1, gone, 1));
    CHECK(g_uiCalls.size() == 2);
    ResetUI();
}

static void OnUIThrows(uint64_t, uint64_t, void*)
{
    throw 1;
}

static void UIStateThrowingCallback()
{
    ResetUI();
    UIState::Register(~0ull, &OnUIThrows, Tag(1));
    UIState::Register(~0ull, &OnUI, Tag(2));

    // The next one is still called, this tick and the next.
    UIState::Publish(PLUGIN_UI_IN_GAMEPLAY);
    CHECK(g_uiCalls.size() == 1);
    UIState::Publish(0);
    CHECK(g_uiCalls.size() == 2);
    UIState::Unregister(&OnUIThrows, Tag(1));
    ResetUI();
}

int main()
{
    RunTest("IsGameThread: false on every thread before the first tick", GameThreadBeforeFirstTick);
    RunTest("IsGameThread: true only on the thread that ticks", GameThreadAfterTick);

    RunTest("IsKeyHeld: single keys", SingleKey);
    RunTest("IsKeyHeld: modifier chords", ModifierChords);
    RunTest("IsKeyHeld: Shift+K with Ctrl also held", ExtraModifierHeld);
    RunTest("IsKeyHeld: a modifier bound as a plain key", ModifierAsKey);
    RunTest("IsKeyHeld: mouse buttons", MouseButtons);
    RunTest("IsKeyHeld: swapped mouse buttons", SwappedMouseButtons);
    RunTest("IsKeyHeld: Tilde follows the keyboard layout", TildeFollowsLayout);
    RunTest("IsKeyHeld: unknown names", UnknownNames);
    RunTest("IsKeyHeld: no focus", NoFocus);
    RunTest("IsKeyHeld: no game window yet", NullWindow);
    RunTest("IsKeyHeld: a key pressed while typing", PressedWhileTyping);
    RunTest("IsKeyHeld: a key held before typing", HeldBeforeTyping);
    RunTest("IsKeyHeld: a key released during a keybind capture", ReleasedDuringCapture);
    RunTest("IsKeyHeld: Escape, typing-exempt keys and mouse buttons", TypingExemptions);
    RunTest("IsKeyHeld: a key with a keybind", KeybindAndPollAgree);
    RunTest("IsKeyHeld: a key nobody watches skips the scan", UnwatchedKeySkipsTheScan);
    RunTest("IsKeyHeld: allocates nothing", AllocatesNothing);
    RunTest("IsKeyHeld: any thread", AnyThread);
    RunTest("ForgetModule: an unloading plugin's keybinds go", ForgetModuleDropsItsKeybinds);
    RunTest("ForgetModule: an unloading plugin's game-thread tasks go", ForgetModuleDropsItsGameThreadTasks);
    RunTest("UIState: mask filter, once per change", UIStateMaskAndOncePerChange);
    RunTest("UIState: first comparison is against 0", UIStateFirstComparisonIsAgainstZero);
    RunTest("UIState: duplicate registration ignored", UIStateDuplicateIgnored);
    RunTest("UIState: unregister inside a callback", UIStateUnregisterInsideCallback);
    RunTest("UIState: an unloading plugin's callbacks go", UIStateForgetModule);
    RunTest("UIState: menu read runs only on demand", UIStateMenuReadDemand);
    RunTest("UIState: menu bits stay set while a screen closes", UIStateMenuBits);
    RunTest("UIState: a throwing callback does not stop the rest", UIStateThrowingCallback);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
