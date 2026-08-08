#include "plugin_log_levels.h"

#include <atomic>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

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

        // Fallback for plugins with no entry of their own. Atomic rather than
        // under g_lock so the fast path can test it without taking anything.
        std::atomic<int> g_wildcard{ kInherit };

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

        g_wildcard.store(kInherit, std::memory_order_release);
    }

    int GetWildcard()
    {
        return g_wildcard.load(std::memory_order_acquire);
    }

    void SetWildcard(int levelOrInherit)
    {
        g_wildcard.store(IsValidLevel(levelOrInherit) ? levelOrInherit : kInherit,
                         std::memory_order_release);
    }

    bool AnyOverrides()
    {
        return g_overrideCount.load(std::memory_order_acquire) != 0 ||
               g_wildcard.load(std::memory_order_acquire) != kInherit;
    }

    LogToFile::Level GetEffective(const char* pluginName)
    {
        const int over = GetOverride(pluginName);
        if (IsValidLevel(over))
            return static_cast<LogToFile::Level>(over);

        const int wildcard = g_wildcard.load(std::memory_order_acquire);
        if (IsValidLevel(wildcard))
            return static_cast<LogToFile::Level>(wildcard);

        return LogToFile::g_minLevel;
    }

    bool ShouldLog(const char* pluginName, LogToFile::Level level)
    {
        // Fast path: nothing is overridden at all, so the global level decides
        // -- no lock, no allocation, no map lookup.
        if (g_overrideCount.load(std::memory_order_acquire) == 0 &&
            g_wildcard.load(std::memory_order_acquire) == kInherit)
            return level >= LogToFile::g_minLevel;

        return level >= GetEffective(pluginName);
    }

    // -----------------------------------------------------------------------
    // Command line
    // -----------------------------------------------------------------------
    namespace
    {
        // Split the command line into argv-style tokens, honouring double
        // quotes so a plugin name with a space survives
        // (-PluginLogLevel="My Plugin:trace"). Hand-rolled rather than
        // CommandLineToArgvW to avoid pulling shell32 into a DLL that runs
        // this early in process startup.
        std::vector<std::wstring> TokenizeCommandLine(const wchar_t* cmdLine)
        {
            std::vector<std::wstring> tokens;
            if (!cmdLine) return tokens;

            std::wstring current;
            bool inQuotes = false;
            bool started  = false;

            for (const wchar_t* p = cmdLine; ; ++p)
            {
                const wchar_t c = *p;
                const bool    end = (c == L'\0');
                const bool    sep = !inQuotes && (c == L' ' || c == L'\t');

                if (end || sep)
                {
                    if (started) tokens.push_back(current);
                    current.clear();
                    started = false;
                    if (end) break;
                    continue;
                }

                started = true;
                if (c == L'"') { inQuotes = !inQuotes; continue; }
                current.push_back(c);
            }

            return tokens;
        }

        std::string NarrowUtf8(const std::wstring& text)
        {
            if (text.empty()) return {};
            const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                                nullptr, 0, nullptr, nullptr);
            if (len <= 0) return {};
            std::string out(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                out.data(), len, nullptr, nullptr);
            return out;
        }

        std::wstring Trim(const std::wstring& text)
        {
            size_t begin = 0;
            size_t end   = text.size();
            while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
            while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) --end;
            return text.substr(begin, end - begin);
        }

        // Strict on purpose: LogToFile::ParseLevel answers Debug for anything it
        // does not recognise, which would turn a typo in a launch argument into
        // a silent, wrong setting that then looks like the feature is broken.
        bool ParseLevelWord(const std::wstring& word, int& outValue)
        {
            if (_wcsicmp(word.c_str(), L"default") == 0 ||
                _wcsicmp(word.c_str(), L"inherit") == 0) { outValue = kInherit;                              return true; }
            if (_wcsicmp(word.c_str(), L"trace")   == 0) { outValue = static_cast<int>(LogToFile::Level::Trace); return true; }
            if (_wcsicmp(word.c_str(), L"debug")   == 0) { outValue = static_cast<int>(LogToFile::Level::Debug); return true; }
            if (_wcsicmp(word.c_str(), L"info")    == 0) { outValue = static_cast<int>(LogToFile::Level::Info);  return true; }
            if (_wcsicmp(word.c_str(), L"warn")    == 0) { outValue = static_cast<int>(LogToFile::Level::Warn);  return true; }
            if (_wcsicmp(word.c_str(), L"error")   == 0) { outValue = static_cast<int>(LogToFile::Level::Error); return true; }
            return false;
        }

        const char* LevelWord(int value)
        {
            switch (value)
            {
            case static_cast<int>(LogToFile::Level::Trace): return "TRACE";
            case static_cast<int>(LogToFile::Level::Debug): return "DEBUG";
            case static_cast<int>(LogToFile::Level::Info):  return "INFO";
            case static_cast<int>(LogToFile::Level::Warn):  return "WARN";
            case static_cast<int>(LogToFile::Level::Error): return "ERROR";
            default:                                        return "DEFAULT";
            }
        }

        // One "<name>:<level>" pair.
        void ApplyPair(const std::wstring& pair)
        {
            const std::wstring trimmed = Trim(pair);
            if (trimmed.empty()) return;

            // Split on the LAST colon: a plugin name is free to contain one,
            // a level name never is.
            const size_t colon = trimmed.find_last_of(L':');
            if (colon == std::wstring::npos)
            {
                LogToFile::Error("[PluginLogLevel] '%ls' is not <plugin>:<level> -- ignored",
                                 trimmed.c_str());
                return;
            }

            const std::wstring name  = Trim(trimmed.substr(0, colon));
            const std::wstring level = Trim(trimmed.substr(colon + 1));

            int value = kInherit;
            if (!ParseLevelWord(level, value))
            {
                LogToFile::Error("[PluginLogLevel] '%ls' is not a level "
                                 "(trace/debug/info/warn/error/default) -- '%ls' ignored",
                                 level.c_str(), trimmed.c_str());
                return;
            }

            if (name.empty())
            {
                LogToFile::Error("[PluginLogLevel] '%ls' has no plugin name -- ignored",
                                 trimmed.c_str());
                return;
            }

            // Written with WriteBypass for the same reason the console does it:
            // a line explaining why a plugin's output is missing is worthless
            // if the setting it describes filters the line out too.
            if (name == L"*" || _wcsicmp(name.c_str(), L"all") == 0)
            {
                SetWildcard(value);
                LogToFile::WriteBypass(LogToFile::Level::Info, "INFO",
                    "[PluginLogLevel] All plugins -> %s (command line)", LevelWord(value));
                return;
            }

            const std::string narrow = NarrowUtf8(name);
            SetOverride(narrow.c_str(), value);
            LogToFile::WriteBypass(LogToFile::Level::Info, "INFO",
                "[PluginLogLevel] %s -> %s (command line)", narrow.c_str(), LevelWord(value));
        }
    }

    void ApplyCommandLine()
    {
        static bool s_applied = false;
        if (s_applied) return;
        s_applied = true;

        static const wchar_t kPrefix[] = L"-PluginLogLevel=";
        const size_t prefixLen = wcslen(kPrefix);

        for (const std::wstring& token : TokenizeCommandLine(GetCommandLineW()))
        {
            // Accept the UE '-' spelling and the '/' one Windows tools use.
            const bool dash  = token.size() > prefixLen && token[0] == L'-';
            const bool slash = token.size() > prefixLen && token[0] == L'/';
            if (!dash && !slash) continue;
            if (_wcsnicmp(token.c_str() + 1, kPrefix + 1, prefixLen - 1) != 0) continue;

            // Comma-separated so one argument can cover several plugins.
            const std::wstring value = token.substr(prefixLen);
            size_t start = 0;
            while (start <= value.size())
            {
                const size_t comma = value.find(L',', start);
                ApplyPair(value.substr(start, comma == std::wstring::npos
                                              ? std::wstring::npos : comma - start));
                if (comma == std::wstring::npos) break;
                start = comma + 1;
            }
        }
    }
}
