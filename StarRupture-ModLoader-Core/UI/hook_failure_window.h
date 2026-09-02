#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// HookFailureWindow
//
// The one-shot popup that tells the user a plugin did not load, and why.
//
// Everything it shows comes from PluginHookReport (plugins/plugin_hook_report.h),
// which the plugin manager fills in while running each plugin's OnPluginLoadHooks
// event. Nothing is recorded here -- this is only the client's view of it, and
// the same report is available on a dedicated server through the `hookfailures`
// console command.
//
// It is not a startup-only popup. client_ui's AnyWorldBeginPlay callback calls
// ShowIfPending() when the main menu world loads, which ARMS it; from then on it
// opens itself whenever a new report is committed. That is what makes it work
// for a plugin hot-loaded or reloaded from the console or the plugin list mid-
// session: those run long after the main menu, and a plugin that silently failed
// to hook is exactly the case where the user is about to wonder why nothing
// happened. A reload that now resolves cleanly commits nothing, so it does not
// re-open the window.
//
// The copy button is the point of the window. A missed AOB is fixed by the
// plugin's author, not by the person looking at the popup, so what they need is
// the pattern, the hook name, the game build and the loader build in one paste.
// ---------------------------------------------------------------------------

namespace UI::HookFailureWindow
{
    // Arm the auto-open. Called when the main menu world begins play, from the
    // game thread -- it only sets a flag; the check itself runs in Render() so
    // that everything which touches the open state stays on the render thread.
    void ShowIfPending();

    // Open it unconditionally -- for the plugin list's "why?" affordance, which
    // has to work after the one-shot popup has been dismissed.
    void Show();

    // Render this frame. Call inside an active ImGui frame; renders nothing
    // when closed.
    void Render();

    // True while the popup is open. ShouldCaptureInput() (client_ui.cpp) needs
    // this or the buttons are unclickable when no other loader window is open.
    bool IsOpen();

#ifdef _DEBUG
    // Fill the report with fake entries and open the window immediately.
    void PopulateTestData();
#endif
}

#endif // MODLOADER_CLIENT_BUILD
