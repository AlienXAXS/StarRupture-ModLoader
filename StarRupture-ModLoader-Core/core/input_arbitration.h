#pragma once

// ---------------------------------------------------------------------------
// Who owns the mouse and the keyboard (client builds)
//
// The decisions behind ShouldCaptureInputNow and the ImGui host's keyboard
// arbitration, as plain functions of their inputs so they can be tested without
// a game, a window or an ImGui context.
//
// A cursor screen is the Escape menu or any game screen that shows the game's
// own cursor (the PAUSE_MENU and CURSOR_UP UI-state bits). There a plugin's
// panel or input-capture token no longer takes exclusive capture: the host
// shares the mouse instead, so ImGui gets only the input that lands on one of
// its windows and the game's menu gets the rest. The loader's own windows keep
// exclusive capture everywhere. In gameplay, with no cursor up, a panel or a
// token captures as before.
//
// Arbitration can flip between a key going down and coming up, and a release
// the game never gets leaves the key or button held. A release for a press
// the game saw therefore always reaches it.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>

namespace InputArbitration
{
    // Exclusive capture: the game gets no input at all and ImGui draws the cursor.
    inline bool IsExclusive(bool loaderWindowOpen, bool pluginCaptures, bool cursorScreenUp)
    {
        return loaderWindowOpen || (pluginCaptures && !cursorScreenUp);
    }

    // Whether a widget window is drawn click-through (ImGuiWindowFlags_NoMouseInputs)
    // while the game's cursor screens share the mouse. A widget with
    // PluginWindowFlags_PauseMenuMouseInputs always takes it. One with
    // PluginWindowFlags_MenuMouseWhereUncovered takes it only where no widget of
    // the game's menu is under the cursor, and is click-through where one is.
    // Any other widget is click-through. Nothing is click-through when the mouse
    // is not shared (exclusive capture, or no cursor screen up).
    inline bool WidgetIsClickThrough(bool mouseShared, bool takesMouseAlways,
                                     bool takesMouseWhereUncovered, bool menuWidgetUnderCursor)
    {
        if (!mouseShared || takesMouseAlways)
            return false;
        return !takesMouseWhereUncovered || menuWidgetUnderCursor;
    }

    // Whether a keyboard message is kept from the game, outside exclusive capture.
    // A focused text field owns the keyboard in either mode, so typing in a
    // plugin's search box never reaches the game's menu. Cooperative mode (a
    // passthrough token) also keeps keys ImGui claims for navigation.
    inline bool SharedKeyboardGoesToImGui(bool cooperative, bool wantCaptureKeyboard, bool wantTextInput)
    {
        return wantTextInput || (cooperative && wantCaptureKeyboard);
    }

    // Whether the arbitration above keeps a keyboard or mouse message from the
    // game: not the release of a press the game saw.
    inline bool ShouldKeepFromGame(bool arbitrationKeeps, bool releaseOfSeenPress)
    {
        return arbitrationKeeps && !releaseOfSeenPress;
    }

    // The keys and buttons whose press reached the game and whose release has
    // not yet (ids are virtual-key codes or key name indices). Lock-free, so
    // the window thread and the game thread can both use it.
    class PressesSeenByGame
    {
    public:
        void Pressed(uint32_t id)
        {
            const uint32_t tag = id + 1; // 0 = free slot
            std::atomic<uint32_t>* free = nullptr;
            for (auto& slot : m_slots)
            {
                const uint32_t v = slot.load(std::memory_order_relaxed);
                if (v == tag) return;
                if (v == 0 && !free) free = &slot;
            }
            uint32_t expected = 0;
            if (free) free->compare_exchange_strong(expected, tag, std::memory_order_relaxed);
        }

        // True if the press was recorded; forgets it either way.
        bool TakeRelease(uint32_t id)
        {
            bool seen = false;
            for (auto& slot : m_slots)
            {
                uint32_t expected = id + 1;
                seen |= slot.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
            }
            return seen;
        }

    private:
        std::atomic<uint32_t> m_slots[32]{};
    };
}
