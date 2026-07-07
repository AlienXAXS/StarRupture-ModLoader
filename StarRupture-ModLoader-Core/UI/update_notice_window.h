#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// UpdateNoticeWindow
//
// A one-shot centered popup that lists the plugins the auto-updater replaced
// during modloader init, so the user knows an update happened even though
// there is no changelog to show.
//
// Flow:
//   1. The auto-updater calls AddUpdatedPlugin() for each DLL it downloads
//      (runs on the init thread, before plugins load).
//   2. client_ui's AnyWorldBeginPlay callback calls ShowIfPending() when the
//      main menu world loads; the window opens only if the list is non-empty
//      and it has not been shown yet this session.
//   3. Render() draws the window inside the shared ImGui frame until the
//      user clicks Close.
//
// Debug builds only: PopulateTestData() fills the list with fake entries and
// re-arms the popup, wired to a test button in the F2 menu's Settings tab.
// ---------------------------------------------------------------------------

namespace UI::UpdateNoticeWindow
{
    // Record one updated plugin. oldVersion may be empty/null when the plugin
    // had no previously stored version (first install through the updater).
    // Thread-safe; called from the init thread.
    void AddUpdatedPlugin(const char* name, const char* oldVersion, const char* newVersion);

    // Open the window if any updates were recorded and it has not been shown
    // yet this session. Called when the main menu world begins play.
    void ShowIfPending();

    // Render this frame. Call inside an active ImGui frame; renders nothing
    // when closed.
    void Render();

#ifdef _DEBUG
    // Fill the list with fake entries and open the window immediately.
    void PopulateTestData();
#endif
}

#endif // MODLOADER_CLIENT_BUILD
