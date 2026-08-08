#include "plugin_log_levels.h"

#include <atomic>
#include <cctype>
#include <string>
#include <unordered_map>

namespace PluginLogLevels
{
    namespace
    {
        SRWLOCK g_lock = SRWLOCK_INIT;

        // Keyed by the lowercased plugin name so lookups match however the UI,
        // the console or the plugin itself happens to have cased it.
        std::unordered_map<std::string, int> g_overrides;

        // Mirrors g_overrides.size() so the no-overrides fast path in
        // ShouldLog() needs neither the lock nor a string allocation. That path
        // is the default configuration and runs on every plugin log line.
        std::atomic<int> g_overrideCount{ 0 };

        std::string NormalizeName(const char* pluginName)
        {
            std::string key = pluginName ? pluginName : "";
            for (char& c : key)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return key;
        }

        bool IsValidLevel(int level)
        {
            return level >= static_cast<int>(LogToFile::Level::Trace) &&
                   level <= static_cast<int>(LogToFile::Level::Error);
        }
    }

    int GetOverride(const char* pluginName)
    {
        if (!pluginName || !*pluginName)          return kInherit;
        if (g_overrideCount.load(std::memory_order_acquire) == 0) return kInherit;

        const std::string key = NormalizeName(pluginName);

        int result = kInherit;
        AcquireSRWLockShared(&g_lock);
        auto it = g_overrides.find(key);
        if (it != g_overrides.end())
            result = it->second;
        ReleaseSRWLockShared(&g_lock);
        return result;
    }

    void SetOverride(const char* pluginName, int levelOrInherit)
    {
        if (!pluginName || !*pluginName)
            return;

        const std::string key = NormalizeName(pluginName);

        AcquireSRWLockExclusive(&g_lock);
        if (IsValidLevel(levelOrInherit))
            g_overrides[key] = levelOrInherit;
        else
            g_overrides.erase(key);   // kInherit, or anything out of range
        g_overrideCount.store(static_cast<int>(g_overrides.size()), std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lock);
    }

    void ClearAll()
    {
        AcquireSRWLockExclusive(&g_lock);
        g_overrides.clear();
        g_overrideCount.store(0, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool AnyOverrides()
    {
        return g_overrideCount.load(std::memory_order_acquire) != 0;
    }

    LogToFile::Level GetEffective(const char* pluginName)
    {
        const int over = GetOverride(pluginName);
        return IsValidLevel(over) ? static_cast<LogToFile::Level>(over)
                                  : LogToFile::g_minLevel;
    }

    bool ShouldLog(const char* pluginName, LogToFile::Level level)
    {
        // Fast path: nobody has set an override, so the global level decides --
        // no lock, no allocation, no map lookup.
        if (g_overrideCount.load(std::memory_order_acquire) == 0)
            return level >= LogToFile::g_minLevel;

        return level >= GetEffective(pluginName);
    }
}
