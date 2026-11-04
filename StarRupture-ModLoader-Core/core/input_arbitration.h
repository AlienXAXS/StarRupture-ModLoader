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
// ---------------------------------------------------------------------------

namespace InputArbitration
{
    // Exclusive capture: the game gets no input at all and ImGui draws the cursor.
    inline bool IsExclusive(bool loaderWindowOpen, bool pluginCaptures, bool cursorScreenUp)
    {
        return loaderWindowOpen || (pluginCaptures && !cursorScreenUp);
    }

    // Whether a keyboard message is kept from the game, outside exclusive capture.
    // A focused text field owns the keyboard in either mode, so typing in a
    // plugin's search box never reaches the game's menu. Cooperative mode (a
    // passthrough token) also keeps keys ImGui claims for navigation.
    inline bool SharedKeyboardGoesToImGui(bool cooperative, bool wantCaptureKeyboard, bool wantTextInput)
    {
        return wantTextInput || (cooperative && wantCaptureKeyboard);
    }
}
