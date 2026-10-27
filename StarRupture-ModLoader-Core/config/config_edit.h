#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "plugins/plugin_interface.h"
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// ConfigEdit
//
// The loader's in-memory copy of each plugin's INI values, and the one path a
// UI uses to change them. The Config tab in the mod loader window draws from
// it; the plugin's own IPluginConfig::Write* calls feed it, so a value a
// plugin writes shows up on the page without the plugin being reloaded.
//
//   Load        read a plugin's INI into the table (replaces what was there).
//   SetLive     change the in-memory value and fire OnConfigChanged. No disk
//               I/O, so it is cheap enough to call on every frame of a drag.
//   Commit      write the value to the INI if it differs from what is on disk,
//               and move the keybind registration and Blocking state when the
//               entry is a Keybind that changed.
//   NoteWritten the plugin wrote this value itself (IPluginConfig::Write*).
//               Updates the table without firing OnConfigChanged: the writer
//               already applied its own change.
//   Get/Snapshot/ChangeCount
//               read side. ChangeCount goes up whenever a plugin's table
//               changes, so a UI that cached a Snapshot knows when to re-read.
//
// Thread-safe. The internal mutex is never held while OnConfigChanged callbacks
// run, so a handler may call back into ConfigEdit (for example to clamp the
// value it was just told about).
// ---------------------------------------------------------------------------

namespace ConfigEdit
{
    // One INI value. Fixed-size so a UI can hand `value` straight to an
    // ImGui text input.
    struct Entry
    {
        char section[64];
        char key[64];
        char value[256];
    };

    // <configDir>\<pluginName>.ini
    bool GetIniPath(const char* pluginName, wchar_t* outPath, size_t outLen);

    // Returns the ConfigEntry for (section, key) from schema, or nullptr.
    const ConfigEntry* FindSchemaEntry(const ConfigSchema* schema, const char* section, const char* key);

    // Blocking owner for one keybind config entry. Per entry rather than per
    // plugin, so two binds in the same plugin that share a combo cannot clear
    // each other's blocking either.
    void SetEntryBlocking(const char* pluginName, const char* section, const char* key,
                          const char* combo, bool blocking);

    // Re-reads the plugin's INI into the table and applies the <key>Blocking
    // flag of every Keybind entry in its schema, so the runtime blocking state
    // is current whenever a UI opens the plugin.
    void Load(const char* pluginName);

    // Copies the plugin's entries, in INI order, into out. Returns the change
    // counter as of the copy.
    unsigned Snapshot(const char* pluginName, std::vector<Entry>& out);

    // Per-plugin change counter. 0 for a plugin that has not been loaded.
    unsigned ChangeCount(const char* pluginName);

    // Copies the current value of (section, key) into out. False if the key is
    // not in the plugin's INI.
    bool Get(const char* pluginName, const char* section, const char* key, char* out, size_t outLen);

    void SetLive(const char* pluginName, const char* section, const char* key, const char* value);
    void Commit(const char* pluginName, const char* section, const char* key);

    // The <key>Blocking toggle of a Keybind entry. SetBlocking writes the INI
    // straight away, the same as the toggle always has.
    bool GetBlocking(const char* pluginName, const char* section, const char* key);
    void SetBlocking(const char* pluginName, const char* section, const char* key, bool blocking);

    // See the header comment. Called by ConfigWriteString for every successful
    // plugin write.
    void NoteWritten(const char* pluginName, const char* section, const char* key, const char* value);
}

#endif // MODLOADER_CLIENT_BUILD
