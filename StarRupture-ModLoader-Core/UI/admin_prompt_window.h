#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// AdminPromptWindow
//
// The window a player sees when a server with no admin asks them to claim it:
// the fixed prompt text, the server's last line, a text box for the claim code
// and the buttons Send and Not me. Once the server ends the prompt it shows the
// last line and one OK button instead.
//
// It holds no state of its own beyond the text box. What to show comes from
// AdminPrompt::GetSnapshot() every frame, and the buttons call
// AdminPrompt::Submit() and AdminPrompt::NotMe() (network_channel/admin_prompt.h),
// which only queue the answer for the game thread. Closing the window with its
// X counts as Not me.
// ---------------------------------------------------------------------------

namespace UI::AdminPromptWindow
{
    // Render this frame. Call inside an active ImGui frame; renders nothing
    // when no prompt is open.
    void Render();

    // True while the prompt is open. ShouldCaptureInputNow() (client_ui.cpp)
    // needs this or the window cannot be clicked or typed into when no other
    // loader window is open.
    bool IsOpen();
}

#endif // MODLOADER_CLIENT_BUILD
