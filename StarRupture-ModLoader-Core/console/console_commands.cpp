#include "console_commands.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>

#include "console/server_console.h"
#include "logging/log.h"
#include "logging/logger.h"
#include "logging/plugin_log_levels.h"
#include "network_channel/network_channel.h"
#include "plugins/plugin_interface.h"
#include "plugins/plugin_manager.h"
#include "utils/game_thread_dispatch.h"

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

#if defined(MODLOADER_CLIENT_BUILD)
#define MODLOADER_BUILD_KIND "client"
#elif defined(MODLOADER_SERVER_BUILD)
#define MODLOADER_BUILD_KIND "server"
#else
#define MODLOADER_BUILD_KIND "generic"
#endif

namespace ModConsole
{
    // -----------------------------------------------------------------------
    // Sink helpers
    // -----------------------------------------------------------------------
    void Sink::Printf(LineKind kind, const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        buf[sizeof(buf) - 1] = '\0';
        Write(kind, buf);
    }

    void Sink::Out(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        buf[sizeof(buf) - 1] = '\0';
        Write(LineKind::Output, buf);
    }

    void Sink::Notice(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        buf[sizeof(buf) - 1] = '\0';
        Write(LineKind::Notice, buf);
    }

    void Sink::Error(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        buf[sizeof(buf) - 1] = '\0';
        Write(LineKind::Error, buf);
    }

    // -----------------------------------------------------------------------
    // Registry
    //
    // unique_ptr entries rather than Command values: Find() hands out pointers
    // that must stay valid, and a plain vector rehomes its elements when it
    // grows.
    // -----------------------------------------------------------------------
    static std::mutex                             s_registryMutex;
    static std::vector<std::unique_ptr<Command>>  s_commands;
    static bool                                   s_builtinsRegistered = false;

    // True when needle matches name, or one of the space-separated aliases.
    static bool MatchesCommand(const Command& cmd, const char* needle)
    {
        if (_stricmp(cmd.name, needle) == 0)
            return true;

        if (!cmd.aliases)
            return false;

        const char* p = cmd.aliases;
        while (*p)
        {
            while (*p == ' ') ++p;
            const char* start = p;
            while (*p && *p != ' ') ++p;

            const size_t len = static_cast<size_t>(p - start);
            if (len > 0 && _strnicmp(start, needle, len) == 0 && needle[len] == '\0')
                return true;
        }
        return false;
    }

    void Register(const Command& cmd)
    {
        std::lock_guard<std::mutex> lk(s_registryMutex);
        for (const auto& existing : s_commands)
        {
            if (_stricmp(existing->name, cmd.name) == 0)
                return;   // already registered
        }
        s_commands.push_back(std::make_unique<Command>(cmd));
    }

    const Command* Find(const char* name)
    {
        if (!name || !*name) return nullptr;

        std::lock_guard<std::mutex> lk(s_registryMutex);
        for (const auto& cmd : s_commands)
        {
            if (MatchesCommand(*cmd, name))
                return cmd.get();
        }
        return nullptr;
    }

    std::vector<const Command*> GetCommands()
    {
        std::lock_guard<std::mutex> lk(s_registryMutex);
        std::vector<const Command*> out;
        out.reserve(s_commands.size());
        for (const auto& cmd : s_commands)
            out.push_back(cmd.get());
        return out;
    }

    // -----------------------------------------------------------------------
    // Tokenizer
    // -----------------------------------------------------------------------
    std::vector<std::string> Tokenize(const std::string& line)
    {
        std::vector<std::string> tokens;
        std::string current;
        bool inQuotes = false;

        for (char c : line)
        {
            if (c == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }
            if (!inQuotes && (c == ' ' || c == '\t'))
            {
                if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }
                continue;
            }
            current.push_back(c);
        }

        if (!current.empty())
            tokens.push_back(current);

        return tokens;
    }

    // -----------------------------------------------------------------------
    // Plugin argument resolution
    // -----------------------------------------------------------------------
    static bool IsAllDigits(const std::string& s)
    {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }

    // Turns a plugin argument into record indices. Accepts "all", a numeric
    // index as printed by `plugins`, a plugin name, or a DLL file name.
    // Reports the failure to the sink and returns false when nothing matched.
    static bool ResolveTargets(const std::string& arg, std::vector<int>& outIndices, Sink& out)
    {
        const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);

        if (_stricmp(arg.c_str(), "all") == 0)
        {
            for (int i = 0; i < total; ++i)
                outIndices.push_back(i);
            if (outIndices.empty())
                out.Error("No plugins are installed.");
            return !outIndices.empty();
        }

        if (IsAllDigits(arg))
        {
            const int index = atoi(arg.c_str());
            if (index < 0 || index >= total)
            {
                out.Error("No plugin at index %d (there %s %d).", index,
                          total == 1 ? "is" : "are", total);
                return false;
            }
            outIndices.push_back(index);
            return true;
        }

        const int index = PluginManager::FindPluginIndex(arg.c_str());
        if (index < 0)
        {
            out.Error("No plugin matching '%s'. Try 'plugins' for the list.", arg.c_str());
            return false;
        }
        outIndices.push_back(index);
        return true;
    }

    // Name for log/output lines. Falls back to the DLL file name, which is all a
    // record has when its load failed before GetPluginInfo.
    static std::string DisplayNameForIndex(int index)
    {
        std::vector<PluginManager::PluginStatus> statuses;
        const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
        if (total <= 0 || index < 0 || index >= total)
            return "?";

        statuses.resize(static_cast<size_t>(total));
        PluginManager::GetAllPluginStatuses(statuses.data(), total);

        const PluginManager::PluginStatus& s = statuses[static_cast<size_t>(index)];
        if (s.name[0])     return s.name;
        if (s.fileName[0]) return s.fileName;
        return "?";
    }

    // -----------------------------------------------------------------------
    // Built-in commands
    // -----------------------------------------------------------------------
    static void Cmd_Help(const std::vector<std::string>& args, Sink& out)
    {
        const std::vector<const Command*> commands = GetCommands();

        if (args.size() >= 2)
        {
            const Command* cmd = Find(args[1].c_str());
            if (!cmd)
            {
                out.Error("Unknown command '%s'.", args[1].c_str());
                return;
            }
            out.Out("%s", cmd->usage ? cmd->usage : cmd->name);
            out.Notice("  %s", cmd->help ? cmd->help : "");
            if (cmd->aliases && *cmd->aliases)
                out.Notice("  aliases: %s", cmd->aliases);
            return;
        }

        out.Notice("Mod loader commands:");
        for (const Command* cmd : commands)
            out.Out("  %-28s %s", cmd->usage ? cmd->usage : cmd->name, cmd->help ? cmd->help : "");
        out.Notice("Type 'help <command>' for detail.");
    }

    static void Cmd_Plugins(const std::vector<std::string>&, Sink& out)
    {
        const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
        if (total <= 0)
        {
            out.Notice("No plugins installed.");
            return;
        }

        std::vector<PluginManager::PluginStatus> statuses(static_cast<size_t>(total));
        PluginManager::GetAllPluginStatuses(statuses.data(), total);

        out.Notice("%-3s %-26s %-12s %-14s %s", "#", "Plugin", "Version", "State", "File");

        for (int i = 0; i < total; ++i)
        {
            const PluginManager::PluginStatus& s = statuses[static_cast<size_t>(i)];

            const char* state = "Unloaded";
            if (s.isWrongTarget)      state = "Wrong target";
            else if (s.isOutOfDate)   state = s.needsModLoaderUpdate ? "Needs loader" : "Needs update";
            else if (s.isLoaded)      state = "Loaded";

            out.Printf(s.isWrongTarget || s.isOutOfDate ? LineKind::Error : LineKind::Output,
                       "%-3d %-26s %-12s %-14s %s",
                       i,
                       s.name[0]     ? s.name    : "?",
                       s.version[0]  ? s.version : "?",
                       state,
                       s.fileName[0] ? s.fileName : "?");
        }
    }

    static void Cmd_Reload(const std::vector<std::string>& args, Sink& out)
    {
        if (args.size() < 2)
        {
            out.Error("Usage: reload <plugin|index|all>");
            return;
        }

        std::vector<int> targets;
        if (!ResolveTargets(args[1], targets, out))
            return;

        for (int index : targets)
        {
            const std::string name = DisplayNameForIndex(index);
            if (PluginManager::ReloadPlugin(index))
                out.Out("Reloaded %s.", name.c_str());
            else
                out.Error("Reload FAILED for %s -- see modloader.log.", name.c_str());
        }
    }

    static void Cmd_Unload(const std::vector<std::string>& args, Sink& out)
    {
        if (args.size() < 2)
        {
            out.Error("Usage: unload <plugin|index|all>");
            return;
        }

        std::vector<int> targets;
        if (!ResolveTargets(args[1], targets, out))
            return;

        for (int index : targets)
        {
            const std::string name = DisplayNameForIndex(index);
            if (PluginManager::UnloadPlugin(index))
                out.Out("Unloaded %s.", name.c_str());
            else
                out.Notice("%s was not loaded.", name.c_str());
        }
    }

    static void Cmd_Load(const std::vector<std::string>& args, Sink& out)
    {
        if (args.size() < 2)
        {
            out.Error("Usage: load <plugin|index|all>");
            return;
        }

        std::vector<int> targets;
        if (!ResolveTargets(args[1], targets, out))
            return;

        const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
        std::vector<PluginManager::PluginStatus> statuses(static_cast<size_t>(total > 0 ? total : 0));
        if (total > 0)
            PluginManager::GetAllPluginStatuses(statuses.data(), total);

        // Loading an existing record and reloading it are the same operation --
        // ReloadPlugin handles a record whose module is already gone. Already-
        // loaded plugins are skipped rather than bounced, so `load all` cannot
        // restart plugins the user never asked it to touch.
        for (int index : targets)
        {
            const std::string name = DisplayNameForIndex(index);

            if (index < total && statuses[static_cast<size_t>(index)].isLoaded)
            {
                out.Notice("%s is already loaded (use 'reload' to restart it).", name.c_str());
                continue;
            }

            if (PluginManager::ReloadPlugin(index))
                out.Out("Loaded %s.", name.c_str());
            else
                out.Error("Load FAILED for %s -- see modloader.log.", name.c_str());
        }
    }

    static void Cmd_Rescan(const std::vector<std::string>&, Sink& out)
    {
        const int loaded = PluginManager::ScanForNewPlugins();
        if (loaded > 0)
            out.Out("Loaded %d new plugin(s).", loaded);
        else
            out.Notice("No new plugin DLLs found.");
    }

    static void Cmd_Version(const std::vector<std::string>&, Sink& out)
    {
        out.Out("StarRupture Mod Loader %s (%s build)", MODLOADER_BUILD_TAG, MODLOADER_BUILD_KIND);
        out.Out("Plugin interface v%d (accepts v%d - v%d)",
                PLUGIN_INTERFACE_VERSION,
                PLUGIN_INTERFACE_VERSION_MIN,
                PLUGIN_INTERFACE_VERSION_MAX);
        out.Out("%d plugin(s) loaded", PluginManager::GetLoadedPluginCount());
    }

    static void Cmd_Clear(const std::vector<std::string>&, Sink& out)
    {
        out.Clear();
    }

    // -----------------------------------------------------------------------
    // loglevel
    // -----------------------------------------------------------------------
    static const char* LevelName(LogToFile::Level level)
    {
        switch (level)
        {
        case LogToFile::Level::Trace: return "TRACE";
        case LogToFile::Level::Debug: return "DEBUG";
        case LogToFile::Level::Info:  return "INFO";
        case LogToFile::Level::Warn:  return "WARN";
        case LogToFile::Level::Error: return "ERROR";
        default:                      return "?";
        }
    }

    // LogToFile::ParseLevel silently returns Debug for anything it does not
    // recognise, which would make a typo look like it worked. Match here
    // instead so an unknown name can be reported as one.
    static bool ParseLevelStrict(const std::string& name, LogToFile::Level& outLevel)
    {
        if (_stricmp(name.c_str(), "trace") == 0) { outLevel = LogToFile::Level::Trace; return true; }
        if (_stricmp(name.c_str(), "debug") == 0) { outLevel = LogToFile::Level::Debug; return true; }
        if (_stricmp(name.c_str(), "info")  == 0) { outLevel = LogToFile::Level::Info;  return true; }
        if (_stricmp(name.c_str(), "warn")  == 0) { outLevel = LogToFile::Level::Warn;  return true; }
        if (_stricmp(name.c_str(), "error") == 0) { outLevel = LogToFile::Level::Error; return true; }
        return false;
    }

    // Per-plugin form of loglevel: sets one plugin's own minimum, or "default"
    // to put it back on the global level. Same registry the client's Logging
    // tab drives -- a dedicated server has no UI, and this is the only way to
    // reach it there.
    static void Cmd_LogLevelForPlugin(const std::string& target, const std::string& levelArg, Sink& out)
    {
        int newValue = PluginLogLevels::kInherit;
        if (_stricmp(levelArg.c_str(), "default") != 0 &&
            _stricmp(levelArg.c_str(), "inherit") != 0)
        {
            LogToFile::Level parsed = LogToFile::Level::Info;
            if (!ParseLevelStrict(levelArg, parsed))
            {
                out.Error("'%s' is not a log level. Use trace, debug, info, warn, error or default.",
                          levelArg.c_str());
                return;
            }
            newValue = static_cast<int>(parsed);
        }

        const char* newName = (newValue == PluginLogLevels::kInherit)
                            ? "DEFAULT" : LevelName(static_cast<LogToFile::Level>(newValue));

        // '*' applies to every plugin record the manager knows about, which is
        // what "all plugins" has to mean here -- the registry itself holds only
        // the plugins someone has already overridden. (The command line's '*'
        // cannot work this way: no plugin has a name yet when it is parsed, so
        // it sets the wildcard fallback instead.)
        if (target == "*" || _stricmp(target.c_str(), "all") == 0)
        {
            // "everything back to normal" has to include a wildcard the command
            // line set, or there is no way to undo -PluginLogLevel=* short of
            // relaunching the game.
            if (newValue == PluginLogLevels::kInherit)
            {
                PluginLogLevels::ClearAll();
                out.Out("All plugins -> DEFAULT.");
                LogToFile::Error("[Console] Plugin log level: all -> DEFAULT");
                return;
            }

            const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
            if (total <= 0)
            {
                out.Error("No plugins to configure.");
                return;
            }

            std::vector<PluginManager::PluginStatus> statuses(total);
            PluginManager::GetAllPluginStatuses(statuses.data(), total);

            int applied = 0;
            for (const auto& s : statuses)
            {
                if (!s.name[0]) continue;
                PluginLogLevels::SetOverride(s.name, newValue);
                ++applied;
            }

            out.Out("%d plugin(s) -> %s.", applied, newName);
            out.Notice("Not saved -- every plugin is back on DEFAULT next start.");
            LogToFile::Error("[Console] Plugin log level: all -> %s", newName);
            return;
        }

        const int index = PluginManager::FindPluginIndex(target.c_str());
        if (index < 0)
        {
            out.Error("No plugin matches '%s'. Try 'plugins' for the list.", target.c_str());
            return;
        }

        // Resolve back to the PluginInfo name: the registry is keyed by it, and
        // the user may well have typed the DLL file name instead.
        const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
        std::vector<PluginManager::PluginStatus> statuses(total > 0 ? total : 0);
        if (total > 0)
            PluginManager::GetAllPluginStatuses(statuses.data(), total);

        if (index >= total || !statuses[index].name[0])
        {
            out.Error("'%s' has never loaded far enough to have a name, so it logs nothing.",
                      target.c_str());
            return;
        }

        const char* pluginName = statuses[index].name;
        const int   previous   = PluginLogLevels::GetOverride(pluginName);
        const char* prevName   = (previous == PluginLogLevels::kInherit)
                               ? "DEFAULT" : LevelName(static_cast<LogToFile::Level>(previous));

        PluginLogLevels::SetOverride(pluginName, newValue);

        out.Out("%s log level %s -> %s.", pluginName, prevName, newName);
        out.Notice("Not saved -- this plugin is back on DEFAULT next start.");
        LogToFile::Error("[Console] Plugin log level: %s %s -> %s", pluginName, prevName, newName);
    }

    static void Cmd_LogLevel(const std::vector<std::string>& args, Sink& out)
    {
        if (args.size() < 2)
        {
            out.Out("Log level is %s.", LevelName(LogToFile::g_minLevel));

            const int wildcard = PluginLogLevels::GetWildcard();
            if (wildcard != PluginLogLevels::kInherit)
                out.Notice("Plugin default is %s (-PluginLogLevel=*), not the level above.",
                           LevelName(static_cast<LogToFile::Level>(wildcard)));

            if (PluginLogLevels::AnyOverrides())
            {
                const int total = PluginManager::GetAllPluginStatuses(nullptr, 0);
                if (total > 0)
                {
                    std::vector<PluginManager::PluginStatus> statuses(total);
                    PluginManager::GetAllPluginStatuses(statuses.data(), total);
                    for (const auto& s : statuses)
                    {
                        if (!s.name[0]) continue;
                        const int over = PluginLogLevels::GetOverride(s.name);
                        if (over == PluginLogLevels::kInherit) continue;
                        out.Out("  %-28s %s", s.name, LevelName(static_cast<LogToFile::Level>(over)));
                    }
                }
            }

            out.Notice("Set it with: loglevel <trace|debug|info|warn|error>");
            out.Notice("Per plugin:  loglevel <plugin|*> <level|default>   (runtime only)");
            return;
        }

        // Two arguments always mean the per-plugin form -- no log level is also
        // a plugin name, so there is nothing to disambiguate.
        if (args.size() >= 3)
        {
            Cmd_LogLevelForPlugin(args[1], args[2], out);
            return;
        }

        LogToFile::Level level = LogToFile::Level::Info;
        if (!ParseLevelStrict(args[1], level))
        {
            out.Error("'%s' is not a log level. Use trace, debug, info, warn or error.", args[1].c_str());
            out.Notice("For one plugin: loglevel <plugin> <level|default>");
            return;
        }

        const char* previous = LevelName(LogToFile::g_minLevel);
        LogToFile::SetLevel(level);

        out.Out("Log level %s -> %s.", previous, LevelName(level));
        out.Notice("Runtime only -- modloader.ini [Logging] Level still decides the next start.");

        // Logged at Error so it lands in the file whatever the new level is:
        // the line explaining why the log suddenly went quiet is worthless if
        // the change itself filters it out.
        LogToFile::Error("[Console] Log level changed %s -> %s", previous, LevelName(level));
    }

#ifndef MODLOADER_CLIENT_BUILD
    // -----------------------------------------------------------------------
    // stop
    //
    // Server/generic builds only. On a client the same key would quit the game
    // out from under the player, and typing 'stop' into the in-game console
    // expecting anything else is a mistake nobody should be able to make.
    // -----------------------------------------------------------------------
    static void Cmd_Stop(const std::vector<std::string>& args, Sink& out)
    {
        const bool force = args.size() >= 2 && _stricmp(args[1].c_str(), "force") == 0;

        if (force)
        {
            out.Error("Forcing exit now. The current save may be incomplete.");
            LogToFile::Error("[Console] 'stop force' -- terminating the process without engine shutdown");
            ExitProcess(0);
            return;   // not reached
        }

        if (!ServerConsole::RequestGracefulProcessExit())
        {
            out.Error("Could not request shutdown -- no console to raise the event on. See modloader.log.");
            return;
        }

        out.Out("Shutdown requested. The engine finishes the current tick, runs its shutdown and exits.");
        out.Notice("If it is still here in a minute, 'stop force' exits immediately (may lose the save).");
    }
#endif

    // -----------------------------------------------------------------------
    // clients -- who is connected, and what they reported having installed
    // -----------------------------------------------------------------------
    static void Cmd_Clients(const std::vector<std::string>& args, Sink& out)
    {
        auto clients = NetworkChannel::GetClientManifests();

        if (clients.empty())
        {
            out.Out("No connected clients. (Only a dedicated server or listen host "
                    "has any; a pure client always reports none.)");
            return;
        }

        // "clients <n>" -- full plugin list for one client.
        if (args.size() >= 2)
        {
            const int idx = atoi(args[1].c_str());
            if (idx < 0 || idx >= static_cast<int>(clients.size()))
            {
                out.Error("No client %d. Use 'clients' to list them (0-%d).",
                          idx, static_cast<int>(clients.size()) - 1);
                return;
            }

            const auto& c = clients[static_cast<size_t>(idx)];
            out.Notice("Client %d: %s", idx,
                       c.playerName.empty() ? "(name not resolved yet)" : c.playerName.c_str());

            if (!c.reported)
            {
                out.Out("  No manifest reported.");
                out.Out("  This client receives NO plugin packets -- either it is not running");
                out.Out("  the mod loader, or it has not finished joining yet.");
                if (c.greetingAttempts > 0)
                    out.Out("  Greeted %d time(s) with no answer so far. A client running the "
                            "loader answers on the first one.", c.greetingAttempts);
                else
                    out.Out("  Not currently being greeted: it either already answered once, or "
                            "we have stopped asking.");
                return;
            }
            if (c.plugins.empty())
            {
                out.Out("  Mod loader present, no plugins loaded.");
                return;
            }

            for (const auto& p : c.plugins)
                out.Out("  %-32s %s", p.name.c_str(), p.version.c_str());
            return;
        }

        // Summary table.
        out.Notice("%-4s %-28s %s", "#", "PLAYER", "PLUGINS");
        for (size_t i = 0; i < clients.size(); ++i)
        {
            const auto& c = clients[i];
            const char* name = c.playerName.empty() ? "(unresolved)" : c.playerName.c_str();

            if (!c.reported && c.greetingAttempts > 0)
                out.Out("%-4zu %-28s greeting, no answer yet (%d)", i, name, c.greetingAttempts);
            else if (!c.reported)
                out.Out("%-4zu %-28s %s", i, name, "no manifest - receives nothing");
            else
                out.Out("%-4zu %-28s %zu", i, name, c.plugins.size());
        }
        out.Out("");
        out.Out("'clients <n>' lists one client's plugins and versions.");
    }

    // -----------------------------------------------------------------------
    // nettest -- round-trip a payload off the server to prove fragmentation works
    // -----------------------------------------------------------------------
    static void Cmd_NetTest(const std::vector<std::string>& args, Sink& out)
    {
        // Default is comfortably over the per-bunch budget, so the default run
        // actually exercises chunking rather than the single-bunch path.
        size_t bytes = 64 * 1024;
        if (args.size() >= 2)
        {
            const long long v = atoll(args[1].c_str());
            if (v <= 0)
            {
                out.Error("Size must be a positive number of bytes.");
                return;
            }
            bytes = static_cast<size_t>(v);
        }

        std::string err;
        if (!NetworkChannel::StartFragmentationTest(bytes, err))
        {
            out.Error("Could not start: %s", err.c_str());
            return;
        }

        out.Notice("Echo test started: %zu bytes sent to the server.", bytes);
        out.Out("The server bounces it back and every byte is verified on return.");
        out.Out("Result is asynchronous -- watch for ECHO TEST PASSED / FAILED here");
        out.Out("and in modloader.log.");
    }

    void RegisterBuiltins()
    {
        {
            std::lock_guard<std::mutex> lk(s_registryMutex);
            if (s_builtinsRegistered)
                return;
            s_builtinsRegistered = true;
        }

        Register({ "help",    "? commands", "help [command]",
                   "List mod loader commands, or explain one",
                   &Cmd_Help,    false });

        Register({ "plugins", "list mods",  "plugins",
                   "List every plugin, its version and state",
                   &Cmd_Plugins, false });

        Register({ "reload",  nullptr,      "reload <plugin|index|all>",
                   "Shut a plugin down, free its DLL and load it again",
                   &Cmd_Reload,  true });

        Register({ "unload",  nullptr,      "unload <plugin|index|all>",
                   "Shut a plugin down and free its DLL",
                   &Cmd_Unload,  true });

        Register({ "load",    nullptr,      "load <plugin|index|all>",
                   "Load a plugin that is currently unloaded",
                   &Cmd_Load,    true });

        Register({ "rescan",  "scan",       "rescan",
                   "Load plugin DLLs added to the Plugins folder since startup",
                   &Cmd_Rescan,  true });

        Register({ "version", "ver",        "version",
                   "Show mod loader build and plugin interface versions",
                   &Cmd_Version, false });

        Register({ "loglevel", "log",       "loglevel [level] | loglevel <plugin|*> <level|default>",
                   "Show or change the log level, globally or for one plugin",
                   &Cmd_LogLevel, false });

        Register({ "clear",   "cls",        "clear",
                   "Wipe the console scrollback",
                   &Cmd_Clear,   false });

        // Reads live actor/connection state, so it must run on the game thread.
        Register({ "clients", "who",        "clients [n]",
                   "List connected clients and the plugins they reported",
                   &Cmd_Clients, true });

        // Touches the net driver and sends on the control channel.
        Register({ "nettest", nullptr,      "nettest [bytes]",
                   "Round-trip a payload off the server to verify fragmentation",
                   &Cmd_NetTest, true });

#ifndef MODLOADER_CLIENT_BUILD
        // Not on the game thread on purpose: a wedged engine is exactly when
        // you most want to stop the server, and a command queued behind a tick
        // that never comes would never run.
        Register({ "stop",    "shutdown",   "stop [force]",
                   "Shut the server down cleanly ('force' exits immediately)",
                   &Cmd_Stop,    false });
#endif
    }

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------
    static void RunHandler(const Command* cmd,
                           const std::vector<std::string>& args,
                           const std::shared_ptr<Sink>& sink)
    {
        // A plugin's PluginInit/PluginShutdown runs underneath these handlers,
        // so a throwing plugin must not take the game thread with it. This does
        // not catch access violations (/EHsc) -- PluginManager wraps those in
        // SEH itself.
        try
        {
            cmd->handler(args, *sink);
        }
        catch (const std::exception& e)
        {
            sink->Error("Command '%s' threw: %s", cmd->name, e.what());
            ModLoaderLogger::LogError(L"[Console] Command '%S' threw an exception", cmd->name);
        }
        catch (...)
        {
            sink->Error("Command '%s' threw an unknown exception.", cmd->name);
            ModLoaderLogger::LogError(L"[Console] Command '%S' threw an unknown exception", cmd->name);
        }
    }

    bool Dispatch(const std::string& line,
                  std::shared_ptr<Sink> sink,
                  std::function<void()> onComplete)
    {
        if (!sink)
            return false;

        std::vector<std::string> args = Tokenize(line);
        if (args.empty())
            return false;

        const Command* cmd = Find(args[0].c_str());
        if (!cmd)
            return false;

        ModLoaderLogger::LogInfo(L"[Console] Executing: %S", line.c_str());

        if (cmd->gameThread && !GameThreadDispatch::IsGameThread())
        {
            // Queued, not run: the game thread picks this up on its next tick.
            // Everything the handler needs is captured by value, so it does not
            // matter how long that takes or whether the caller has moved on.
            GameThreadDispatch::PostVoid([cmd, args, sink, onComplete]()
            {
                RunHandler(cmd, args, sink);
                if (onComplete) onComplete();
            });
            return true;
        }

        RunHandler(cmd, args, sink);
        if (onComplete) onComplete();
        return true;
    }
}
