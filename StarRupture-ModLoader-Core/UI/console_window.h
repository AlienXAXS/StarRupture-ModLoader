#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// ConsoleWindow
//
// An ImGui developer console, styled after the engine's own: pinned to the top
// of the viewport, full width.
//
// A submitted line goes to the mod loader's own command registry first
// (console/console_commands.h -- help, plugins, reload, ...), shared with the
// -console window on server builds. Anything the registry does not recognise is
// executed through Hooks::ConsoleCommand (APlayerController::ConsoleCommand) on
// the game thread, and whatever the engine returns is echoed back into the
// scrollback. Prefixing a line with '!' skips the registry, for when a mod
// loader command shadows an engine one of the same name.
//
// This deliberately does not try to revive the engine's own UConsole. On this
// Shipping build the UConsole class is intact but every piece of glue that
// drives it is compiled out -- no ViewportConsole dispatch in
// UGameViewportClient::InputKey, no callers of UConsole::PostRender_Console,
// and nothing reads UInputSettings::ConsoleKeys. Command execution itself is
// untouched, so driving it from our own UI is both simpler and far less
// fragile across game updates.
//
// Always available; the open key (default Tilde) is configurable via
// modloader.ini [Console] OpenKey.
// ---------------------------------------------------------------------------

namespace UI::ConsoleWindow
{
    // Read [Console] settings from modloader.ini and register the open key.
    // Call once during client UI init.
    void Load(const wchar_t* iniPath);

    // UE key name for the configured open key (e.g. "Tilde"), for UI display.
    const char* GetOpenKeyName();

    void Open();
    void Close();
    void Toggle();
    bool IsOpen();

    // Draw. Safe to call every frame; returns immediately when closed.
    void Render();
}

#endif // MODLOADER_CLIENT_BUILD
