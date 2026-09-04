#pragma once

struct IPluginConsole;

// ---------------------------------------------------------------------------
// PluginConsole -- the plugin-facing front of ModConsole (v63)
//
// Bridges IPluginConsole (plugins/plugin_interface.h) onto the command registry
// in console/console_commands.h, so a command registered by a plugin appears in
// both console front-ends exactly like a built-in, and a plugin can run any
// command line and capture its output through a callback instead of a window.
//
// Two things this layer owns that the registry deliberately does not:
//
//  - The trampoline context. ModConsole calls a ContextHandler; the context is
//    the plugin's own handler plus its userData, allocated here and kept in an
//    arena that is never freed, only marked dead and recycled for the same
//    owner+name. A queued game-thread dispatch can still be holding that
//    pointer when the plugin is unloaded, and a freed context would be read
//    before anything had a chance to notice the plugin was gone.
//
//  - Liveness checks. Every call into a plugin goes through a handler address
//    that must still belong to a loaded module, the same check the keybind
//    registry makes for the same reason.
// ---------------------------------------------------------------------------
namespace PluginConsole
{
    // The interface handed to plugins as hooks->Console. Never null.
    IPluginConsole* GetInterface();

    // Drop every command this plugin registered and kill its trampoline
    // contexts. Called by PluginManager before FreeLibrary -- a command left
    // registered past that point is a handler pointer into an unmapped module.
    void ForgetPlugin(const char* pluginName);
}
