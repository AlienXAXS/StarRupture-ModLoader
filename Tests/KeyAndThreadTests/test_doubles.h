#pragma once

#include <windows.h>
#include <atomic>

// What the test doubles in test_doubles.cpp report, so the tests can hold
// keys and move focus without touching the real keyboard or desktop.
namespace TestDoubles
{
    // Hold or release a virtual key, as GetAsyncKeyState will report it.
    void SetKeyDown(int vk, bool down);
    void ReleaseAllKeys();

    // GetForegroundWindow's answer, and the window the overlay reports as the
    // game's (UI::ImGuiBackend::GetGameWindow; null before the first Present).
    extern std::atomic<HWND> foreground;
    extern std::atomic<HWND> gameWindow;

    // GetSystemMetrics(SM_SWAPBUTTON): the player swapped the mouse buttons.
    extern std::atomic<bool> buttonsSwapped;

    // The typing gate's two sources: a game text widget has keyboard focus
    // (Hooks::TextInputFocus::IsGameTextInputFocused), an ImGui text field is
    // active (UI::ImGuiBackend::IsTextInputActive). gameTextChecks counts the
    // first one's calls, the costly one in the game.
    extern std::atomic<bool> gameTextFocused;
    extern std::atomic<bool> imguiTextActive;
    extern std::atomic<long> gameTextChecks;
}
