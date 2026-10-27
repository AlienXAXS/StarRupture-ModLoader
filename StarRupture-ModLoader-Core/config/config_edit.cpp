#include "pch.h"
#include "config_edit.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "config_manager.h"
#include "UI/plugin_panel_registry.h"
#include "hooks/input/keybind_registry.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace ConfigEdit
{
    namespace
    {
        // Plugin names compare the way FireConfigChanged and the INI file
        // system do: ignoring case.
        struct NameLess
        {
            bool operator()(const std::string& a, const std::string& b) const
            {
                return _stricmp(a.c_str(), b.c_str()) < 0;
            }
        };

        struct Table
        {
            std::vector<Entry> entries;
            unsigned           changeCount = 0;
        };

        std::mutex                                 s_mutex;
        std::map<std::string, Table, NameLess>     s_tables;

        // INI section and key names are case-insensitive.
        bool SameEntry(const Entry& e, const char* section, const char* key)
        {
            return _stricmp(e.section, section) == 0 && _stricmp(e.key, key) == 0;
        }

        Entry* FindEntry(std::vector<Entry>& entries, const char* section, const char* key)
        {
            for (Entry& e : entries)
                if (SameEntry(e, section, key))
                    return &e;
            return nullptr;
        }

        Entry* FindEntry(Table& t, const char* section, const char* key)
        {
            return FindEntry(t.entries, section, key);
        }

        void BlockingKeyName(const char* key, char* out, size_t outLen)
        {
            snprintf(out, outLen, "%sBlocking", key);
        }

        bool ToBool(const char* value)
        {
            return atoi(value) != 0;
        }

        // Parses every section + key=value pair of the plugin's INI, in file
        // order. Reads the disk, so never call it with s_mutex held.
        std::vector<Entry> ReadIni(const char* pluginName)
        {
            std::vector<Entry> out;

            wchar_t iniPath[MAX_PATH];
            if (!GetIniPath(pluginName, iniPath, MAX_PATH))
                return out;

            // Enumerate section names (double-NUL terminated list)
            wchar_t sectionBuf[4096] = {};
            GetPrivateProfileSectionNamesW(sectionBuf, ARRAYSIZE(sectionBuf), iniPath);

            for (const wchar_t* sec = sectionBuf; *sec; sec += wcslen(sec) + 1)
            {
                // Read all key=value pairs in this section
                wchar_t kvBuf[8192] = {};
                GetPrivateProfileSectionW(sec, kvBuf, ARRAYSIZE(kvBuf), iniPath);

                for (const wchar_t* kv = kvBuf; *kv; kv += wcslen(kv) + 1)
                {
                    const wchar_t* eq = wcschr(kv, L'=');
                    if (!eq) continue;

                    Entry entry = {};
                    snprintf(entry.section, sizeof(entry.section), "%ls", sec);
                    int keyLen = static_cast<int>(eq - kv);
                    if (keyLen <= 0 || keyLen >= static_cast<int>(sizeof(entry.key))) continue;
                    snprintf(entry.key, sizeof(entry.key), "%.*ls", keyLen, kv);
                    snprintf(entry.value, sizeof(entry.value), "%ls", eq + 1);

                    out.push_back(entry);
                }
            }
            return out;
        }

        // Loads the plugin's table if it has none yet.
        void EnsureLoaded(const char* pluginName)
        {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (s_tables.find(pluginName) != s_tables.end())
                    return;
            }
            Load(pluginName);
        }

        // Stores value into the table, adding the entry if the plugin's INI
        // did not have it. A new entry goes after the last one of its section,
        // which is where the INI file itself would put it. Returns true if
        // the table changed. Call with s_mutex held.
        bool Store(Table& t, const char* section, const char* key, const char* value)
        {
            if (!section || !key || !value) return false;
            if (!key[0] || strlen(key) >= sizeof(Entry::key)) return false;

            if (Entry* e = FindEntry(t, section, key))
            {
                if (strcmp(e->value, value) == 0)
                    return false;
                strncpy_s(e->value, value, _TRUNCATE);
            }
            else
            {
                Entry entry = {};
                strncpy_s(entry.section, section, _TRUNCATE);
                strncpy_s(entry.key, key, _TRUNCATE);
                strncpy_s(entry.value, value, _TRUNCATE);

                auto pos = t.entries.end();
                for (auto it = t.entries.end(); it != t.entries.begin();)
                {
                    --it;
                    if (_stricmp(it->section, entry.section) == 0)
                    {
                        pos = it + 1;
                        break;
                    }
                }
                t.entries.insert(pos, entry);
            }
            ++t.changeCount;
            return true;
        }
    }

    bool GetIniPath(const char* pluginName, wchar_t* outPath, size_t outLen)
    {
        const wchar_t* configDir = ModLoaderLogger::GetConfigDirectory();
        if (!configDir || !pluginName) return false;
        swprintf_s(outPath, outLen, L"%s\\%S.ini", configDir, pluginName);
        return true;
    }

    const ConfigEntry* FindSchemaEntry(const ConfigSchema* schema, const char* section, const char* key)
    {
        if (!schema) return nullptr;
        for (int i = 0; i < schema->entryCount; ++i)
        {
            const ConfigEntry& e = schema->entries[i];
            if (strcmp(e.section, section) == 0 && strcmp(e.key, key) == 0)
                return &e;
        }
        return nullptr;
    }

    void SetEntryBlocking(const char* pluginName, const char* section, const char* key,
                          const char* combo, bool blocking)
    {
        char owner[256];
        snprintf(owner, sizeof(owner), "%s|%s|%s", pluginName, section, key);
        Hooks::Input::SetComboBlocking(owner, combo, blocking);
    }

    void Load(const char* pluginName)
    {
        if (!pluginName) return;

        std::vector<Entry> entries = ReadIni(pluginName);

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            Table& t = s_tables[pluginName];
            t.entries = entries;
            ++t.changeCount;
        }

        // Apply the companion <key>Blocking flag of every Keybind entry, so
        // the runtime blocking state is current whenever a UI opens the plugin.
        const ConfigSchema* schema = ModLoaderLogger::GetPluginSchema(pluginName);
        if (!schema) return;

        // Walk the schema rather than the INI, and look the values up the way
        // the INI does, ignoring case: a hand-edited [input] or boost still
        // applies.
        for (int i = 0; i < schema->entryCount; ++i)
        {
            const ConfigEntry& sch = schema->entries[i];
            if (sch.type != ConfigValueType::Keybind) continue;
            if (!sch.section || !sch.key) continue;

            const Entry* combo = FindEntry(entries, sch.section, sch.key);
            if (!combo || !combo->value[0]) continue;

            char blockingKey[128];
            BlockingKeyName(sch.key, blockingKey, sizeof(blockingKey));
            const Entry* flag = FindEntry(entries, sch.section, blockingKey);
            SetEntryBlocking(pluginName, sch.section, sch.key, combo->value,
                             flag && ToBool(flag->value));
        }
    }

    unsigned Snapshot(const char* pluginName, std::vector<Entry>& out)
    {
        out.clear();
        if (!pluginName) return 0;
        EnsureLoaded(pluginName);

        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_tables.find(pluginName);
        if (it == s_tables.end()) return 0;
        out = it->second.entries;
        return it->second.changeCount;
    }

    unsigned ChangeCount(const char* pluginName)
    {
        if (!pluginName) return 0;
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_tables.find(pluginName);
        return (it != s_tables.end()) ? it->second.changeCount : 0;
    }

    bool Get(const char* pluginName, const char* section, const char* key, char* out, size_t outLen)
    {
        if (!pluginName || !section || !key || !out || outLen == 0) return false;
        EnsureLoaded(pluginName);

        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_tables.find(pluginName);
        if (it == s_tables.end()) return false;
        const Entry* e = FindEntry(it->second, section, key);
        if (!e) return false;
        strncpy_s(out, outLen, e->value, _TRUNCATE);
        return true;
    }

    void SetLive(const char* pluginName, const char* section, const char* key, const char* value)
    {
        if (!pluginName || !section || !key || !value) return;
        EnsureLoaded(pluginName);

        char stored[sizeof(Entry::value)];
        strncpy_s(stored, value, _TRUNCATE);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            Store(s_tables[pluginName], section, key, stored);
        }

        // Outside the lock: a handler is allowed to call back in.
        UI::PluginPanelRegistry::FireConfigChanged(pluginName, section, key, stored);
    }

    void Commit(const char* pluginName, const char* section, const char* key)
    {
        if (!pluginName || !section || !key) return;
        EnsureLoaded(pluginName);

        char value[sizeof(Entry::value)];
        if (!Get(pluginName, section, key, value, sizeof(value)))
            return;

        wchar_t iniPath[MAX_PATH];
        if (!GetIniPath(pluginName, iniPath, MAX_PATH)) return;

        wchar_t wsec[64], wkey[64], wval[256];
        swprintf_s(wsec, L"%S", section);
        swprintf_s(wkey, L"%S", key);
        swprintf_s(wval, L"%S", value);

        // Nothing to write if the file already says this.
        static const wchar_t kAbsent[] = L"\x1f<absent>\x1f";
        wchar_t onDisk[1024] = {};
        GetPrivateProfileStringW(wsec, wkey, kAbsent, onDisk, ARRAYSIZE(onDisk), iniPath);
        const bool wasOnDisk = wcscmp(onDisk, kAbsent) != 0;
        if (wasOnDisk && wcscmp(onDisk, wval) == 0)
            return;

        WritePrivateProfileStringW(wsec, wkey, wval, iniPath);

        // A Keybind whose value changed: live-rebind any active keybind
        // registrations for this plugin and transfer blocking state.
        const ConfigEntry* entry = FindSchemaEntry(ModLoaderLogger::GetPluginSchema(pluginName), section, key);
        if (entry && entry->type == ConfigValueType::Keybind)
        {
            char oldValue[256] = {};
            if (wasOnDisk)
                snprintf(oldValue, sizeof(oldValue), "%ls", onDisk);

            // Transfer blocking state from the old combo to the new one.
            // Read from INI (the ground truth) rather than the runtime map so this
            // works correctly even if the map entry was never explicitly set.
            wchar_t wblkKey[128];
            swprintf_s(wblkKey, L"%SBlocking", key);
            const bool wasBlocking = (GetPrivateProfileIntW(wsec, wblkKey, 0, iniPath) != 0);

            // Withdraw only THIS entry's claim on the old combo -- another
            // plugin still bound to it keeps its own blocking.
            SetEntryBlocking(pluginName, section, key, oldValue, false);
            SetEntryBlocking(pluginName, section, key, value, wasBlocking);

            Hooks::Input::UpdateKeybindByName(pluginName, oldValue, value);
        }
    }

    bool GetBlocking(const char* pluginName, const char* section, const char* key)
    {
        char blockingKey[128];
        BlockingKeyName(key, blockingKey, sizeof(blockingKey));

        char value[sizeof(Entry::value)];
        return Get(pluginName, section, blockingKey, value, sizeof(value)) && ToBool(value);
    }

    void SetBlocking(const char* pluginName, const char* section, const char* key, bool blocking)
    {
        char combo[sizeof(Entry::value)] = {};
        Get(pluginName, section, key, combo, sizeof(combo));

        char blockingKey[128];
        BlockingKeyName(key, blockingKey, sizeof(blockingKey));

        wchar_t iniPath[MAX_PATH];
        if (GetIniPath(pluginName, iniPath, MAX_PATH))
        {
            wchar_t wsec[64], wblkKey[128];
            swprintf_s(wsec, L"%S", section);
            swprintf_s(wblkKey, L"%S", blockingKey);
            WritePrivateProfileStringW(wsec, wblkKey, blocking ? L"1" : L"0", iniPath);
        }

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            Store(s_tables[pluginName], section, blockingKey, blocking ? "1" : "0");
        }

        SetEntryBlocking(pluginName, section, key, combo, blocking);
    }

    void NoteWritten(const char* pluginName, const char* section, const char* key, const char* value)
    {
        if (!pluginName) return;

        // A plugin nobody has opened has no table to keep current; Load reads
        // the INI, which already has this value.
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_tables.find(pluginName);
        if (it != s_tables.end())
            Store(it->second, section, key, value);
    }
}

#endif // MODLOADER_CLIENT_BUILD
