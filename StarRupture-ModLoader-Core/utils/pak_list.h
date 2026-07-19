#pragma once

#include <string>

// ---------------------------------------------------------------------------
// Pak file inventory
//
// Scans the game's Content\Paks directory tree for *.pak files, logs the
// result to modloader.log, and caches a text summary for the crash dialog.
//
// Rationale: by far the most common "modloader crash" report is actually an
// out-of-date pak mod dropped into Content\Paks\~mods (or LogicMods). Putting
// the pak list in the crash details makes that immediately visible.
//
// CaptureAndLog() is called once after engine init (Stage 2), when the game
// has mounted its paks. The summary flags paks living outside the base Paks
// folder (subdirectories like ~mods) as likely mod paks.
// ---------------------------------------------------------------------------

namespace PakList
{
    // Scan Content\Paks, write the inventory to modloader.log, and cache the
    // summary. Safe to call more than once (rescans and replaces the cache).
    void CaptureAndLog();

    // CRLF-formatted summary of the last capture, for the crash dialog.
    // Empty string if CaptureAndLog() has not run (or found nothing).
    const std::wstring& GetSummary();
}
