#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <string>

// ---------------------------------------------------------------------------
// Crash dialog (client only)
//
// A proper Win32 dialog shown by the crash reporter hook instead of a plain
// MessageBox. Built from an in-memory DLGTEMPLATE (no .rc resources needed,
// which matters because this code runs inside a crashing process and must
// not depend on resource loading from a partially-torn-down module).
//
// Contents:
//   - Explanation text: the ModLoader may not be the cause of the crash, and
//     CreepyJar do not look at logs/saves from modded clients.
//   - A read-only, selectable/copyable edit box with the exception details
//     and stack trace.
//   - Buttons to copy the details and to open ModLoader.log and
//     StarRupture.log directly.
// ---------------------------------------------------------------------------

namespace Hooks::CrashDialog
{
    // Shows the modal crash dialog. Blocks until the user closes it.
    // detailsText: exception summary + stack trace (CRLF line endings
    // preferred -- edit controls do not render bare '\n' as line breaks).
    // Falls back to a plain MessageBox if dialog creation fails.
    void Show(const std::wstring& detailsText);
}

#endif // MODLOADER_CLIENT_BUILD
