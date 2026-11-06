#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// UpdateNoticeWindow
//
// A one-shot centered popup that lists the plugins the auto-updater replaced
// during modloader init, so the user knows an update happened. A plugin whose
// update manifest carries a "notes" string also gets those notes listed under
// its row.
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
    // notes may be empty/null; otherwise it holds one change per line,
    // separated by '\n', and is shown as bullets under the plugin.
    // Thread-safe; called from the init thread.
    void AddUpdatedPlugin(const char* name, const char* oldVersion, const char* newVersion,
                          const char* notes);

    // Run `fn` once, on the game thread, when the window actually opens. The
    // loader uses it to record that its release notes were shown. Replaces any
    // earlier callback; never called when the window does not open.
    void SetOnShown(std::function<void()> fn);

    // Open the window if any updates were recorded and it has not been shown
    // yet this session. Called when the main menu world begins play.
    void ShowIfPending();

    // Render this frame. Call inside an active ImGui frame; renders nothing
    // when closed.
    void Render();

    // True while the popup is open. Needed so ShouldCaptureInput() (client_ui.cpp)
    // routes mouse input to ImGui for this window even when the main F2 menu and
    // all plugin panels are closed -- otherwise its Close button is unclickable.
    bool IsOpen();

    // Groups `items` whose non-empty notes are word for word the same, in order
    // of first appearance, so the window lists such plugins on one line with the
    // shared notes once. An item with no notes, or notes of its own, is a group
    // of one. `notesOf` maps an item to its notes (any type with ==, empty()).
    template <class T, class NotesOf>
    std::vector<std::vector<const T*>> GroupByNotes(const std::vector<T>& items, NotesOf notesOf)
    {
        std::vector<std::vector<const T*>> groups;
        for (const T& item : items)
        {
            std::vector<const T*>* group = nullptr;
            if (!notesOf(item).empty())
            {
                for (auto& g : groups)
                {
                    if (notesOf(*g.front()) == notesOf(item)) { group = &g; break; }
                }
            }
            if (group)
                group->push_back(&item);
            else
                groups.push_back({ &item });
        }
        return groups;
    }

#ifdef _DEBUG
    // Fill the list with fake entries and open the window immediately.
    void PopulateTestData();
#endif
}

#endif // MODLOADER_CLIENT_BUILD
