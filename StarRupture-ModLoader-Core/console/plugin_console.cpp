#include "plugin_console.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "console/console_commands.h"
#include "logging/logger.h"
#include "plugins/plugin_interface.h"

namespace PluginConsole
{
    // -----------------------------------------------------------------------
    // A handler address that no longer belongs to a loaded module cannot be
    // called, whatever any bookkeeping says. Same check, and the same crash
    // class, as the keybind registry's CallbackStillMapped.
    // -----------------------------------------------------------------------
    static bool StillMapped(const void* fn)
    {
        if (!fn) return false;

        HMODULE owner = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(fn), &owner))
            return false;

        return owner != nullptr;
    }

    // -----------------------------------------------------------------------
    // Trampoline contexts
    //
    // ModConsole stores a void* per plugin command; this is what it points at.
    // Entries are never freed -- a game-thread dispatch queued a tick ago can
    // still be carrying the pointer when the plugin is unloaded, and freeing it
    // would turn "the plugin is gone" into a read of freed memory. Unregister
    // clears `alive` instead, and a later re-registration of the same
    // owner+name reuses the entry, so a plugin reloaded a hundred times costs
    // one context per command rather than a hundred.
    // -----------------------------------------------------------------------
    struct CommandContext
    {
        std::string          owner;
        std::string          name;
        PluginConsoleHandler handler = nullptr;
        void*                userData = nullptr;
        std::atomic<bool>    alive{ false };
    };

    static std::mutex                                   s_contextMutex;
    static std::vector<std::unique_ptr<CommandContext>> s_contexts;

    // What a context held before AcquireContext overwrote it, so a refused
    // registration can put it back. Refusal is usually a name collision, and
    // one of the things that collides with a name is the SAME plugin's own
    // still-registered command of that name -- killing the context there would
    // break a command that is working.
    struct ContextSnapshot
    {
        PluginConsoleHandler handler  = nullptr;
        void*                userData = nullptr;
        bool                 alive    = false;
    };

    static CommandContext* AcquireContext(const char* owner, const char* name,
                                          PluginConsoleHandler handler, void* userData,
                                          ContextSnapshot& previous)
    {
        std::lock_guard<std::mutex> lk(s_contextMutex);

        CommandContext* slot = nullptr;
        for (auto& c : s_contexts)
        {
            if (_stricmp(c->owner.c_str(), owner) == 0 && _stricmp(c->name.c_str(), name) == 0)
            {
                slot = c.get();
                break;
            }
        }

        if (!slot)
        {
            s_contexts.push_back(std::make_unique<CommandContext>());
            slot = s_contexts.back().get();
            slot->owner = owner;
            slot->name  = name;
        }

        previous.handler  = slot->handler;
        previous.userData = slot->userData;
        previous.alive    = slot->alive.load();

        slot->handler  = handler;
        slot->userData = userData;
        slot->alive.store(true);
        return slot;
    }

    static void RestoreContext(CommandContext* ctx, const ContextSnapshot& previous)
    {
        if (!ctx) return;

        std::lock_guard<std::mutex> lk(s_contextMutex);
        ctx->handler  = previous.handler;
        ctx->userData = previous.userData;
        ctx->alive.store(previous.alive);
    }

    static void KillContexts(const char* owner, const char* name)
    {
        std::lock_guard<std::mutex> lk(s_contextMutex);
        for (auto& c : s_contexts)
        {
            if (_stricmp(c->owner.c_str(), owner) != 0) continue;
            if (name && _stricmp(c->name.c_str(), name) != 0) continue;
            c->alive.store(false);
        }
    }

    // -----------------------------------------------------------------------
    // The ModConsole handler every plugin command is registered with.
    // -----------------------------------------------------------------------
    static void CommandTrampoline(const std::vector<std::string>& args,
                                  ModConsole::Sink& out, void* context)
    {
        CommandContext* ctx = static_cast<CommandContext*>(context);
        if (!ctx || !ctx->alive.load())
            return;

        PluginConsoleHandler handler = ctx->handler;
        if (!StillMapped(reinterpret_cast<const void*>(handler)))
        {
            out.Error("Command handler for '%s' is no longer loaded.",
                      args.empty() ? ctx->name.c_str() : args[0].c_str());
            ModLoaderLogger::LogWarn(L"[PluginConsole] '%S' from plugin '%S' points into an unmapped module",
                                     ctx->name.c_str(), ctx->owner.c_str());
            return;
        }

        std::vector<const char*> argv;
        argv.reserve(args.size());
        for (const std::string& a : args)
            argv.push_back(a.c_str());

        handler(argv.empty() ? nullptr : argv.data(),
                static_cast<int>(argv.size()),
                static_cast<PluginConsoleSink>(&out),
                ctx->userData);
    }

    // -----------------------------------------------------------------------
    // Sink for Execute -- forwards each line to the calling plugin's callback.
    //
    // Held by shared_ptr for the whole life of the dispatched command, so it
    // outlives the Execute call for a game-thread command; both callbacks are
    // re-checked against the loaded modules before every use, because the
    // plugin that asked can be unloaded while its command is still queued.
    // -----------------------------------------------------------------------
    class CallbackSink : public ModConsole::Sink
    {
    public:
        CallbackSink(std::string owner, PluginConsoleOutputCallback onLine, void* userData)
            : m_owner(std::move(owner)), m_onLine(onLine), m_userData(userData) {}

        void Write(ModConsole::LineKind kind, const char* text) override
        {
            if (!m_onLine || !text) return;
            if (!StillMapped(reinterpret_cast<const void*>(m_onLine)))
            {
                m_onLine = nullptr;   // the plugin went away mid-command
                ModLoaderLogger::LogWarn(L"[PluginConsole] Dropping output for '%S': callback is unmapped",
                                         m_owner.c_str());
                return;
            }

            m_onLine(static_cast<PluginConsoleLineKind>(static_cast<int>(kind)), text, m_userData);
        }

    private:
        std::string                 m_owner;
        PluginConsoleOutputCallback m_onLine;
        void*                       m_userData;
    };

    // -----------------------------------------------------------------------
    // IPluginConsole
    // -----------------------------------------------------------------------
    static bool ConsoleRegisterCommand(const IPluginSelf* self, const PluginConsoleCommandDesc* desc)
    {
        if (!self || !self->name || !desc || !desc->name || !*desc->name || !desc->handler)
        {
            ModLoaderLogger::LogWarn(L"[PluginConsole] RegisterCommand: null or incomplete argument");
            return false;
        }

        // A name with whitespace in it can never be typed as one token, so it
        // would register a command nobody can reach.
        for (const char* p = desc->name; *p; ++p)
        {
            if (*p == ' ' || *p == '\t')
            {
                ModLoaderLogger::LogWarn(L"[PluginConsole] '%S' asked for command name '%S', which contains a space",
                                         self->name, desc->name);
                return false;
            }
        }

        // Built-ins first, always. Both front-ends register them lazily
        // ("whichever comes up first"), and a plugin loading before either
        // console exists would otherwise be able to take `reload` -- at which
        // point RegisterBuiltins silently skips the real one, because it
        // ignores duplicates to stay idempotent. Cheap: one bool after the
        // first call.
        ModConsole::RegisterBuiltins();

        ContextSnapshot previous;
        CommandContext* ctx = AcquireContext(self->name, desc->name, desc->handler,
                                             desc->userData, previous);

        ModConsole::Command cmd{};
        cmd.name       = desc->name;
        cmd.aliases    = desc->aliases;
        cmd.usage      = desc->usage;
        cmd.help       = desc->help;
        cmd.gameThread = desc->gameThread;
        cmd.ctxHandler = CommandTrampoline;
        cmd.context    = ctx;

        if (!ModConsole::RegisterPluginCommand(self->name, cmd))
        {
            RestoreContext(ctx, previous);
            ModLoaderLogger::LogWarn(L"[PluginConsole] '%S' could not register command '%S' -- the name or one of its aliases is already taken",
                                     self->name, desc->name);
            return false;
        }

        ModLoaderLogger::LogInfo(L"[PluginConsole] '%S' registered console command '%S'%s",
                                 self->name, desc->name,
                                 desc->gameThread ? L" (game thread)" : L"");
        return true;
    }

    static bool ConsoleUnregisterCommand(const IPluginSelf* self, const char* name)
    {
        if (!self || !self->name || !name || !*name) return false;

        const bool removed = ModConsole::UnregisterPluginCommand(self->name, name);
        KillContexts(self->name, name);
        return removed;
    }

    static int ConsoleUnregisterAllCommands(const IPluginSelf* self)
    {
        if (!self || !self->name) return 0;

        const int removed = ModConsole::ForgetPluginCommands(self->name);
        KillContexts(self->name, nullptr);
        return removed;
    }

    static bool ConsoleHasCommand(const char* name)
    {
        return ModConsole::Exists(name);
    }

    static ModConsole::Sink* ResolveSink(PluginConsoleSink sink, const wchar_t* what)
    {
        ModConsole::Sink* s = static_cast<ModConsole::Sink*>(sink);
        if (!s || !ModConsole::IsSinkActive(s))
        {
            ModLoaderLogger::LogWarn(L"[PluginConsole] %s through a sink that is no longer being served -- write dropped. "
                                     L"A sink is only valid inside the handler it was passed to.", what);
            return nullptr;
        }
        return s;
    }

    static void ConsoleWrite(PluginConsoleSink sink, PluginConsoleLineKind kind, const char* text)
    {
        if (!text) return;

        ModConsole::Sink* s = ResolveSink(sink, L"Write");
        if (!s) return;

        s->Write(static_cast<ModConsole::LineKind>(static_cast<int>(kind)), text);
    }

    static void ConsolePrintf(PluginConsoleSink sink, PluginConsoleLineKind kind, const char* format, ...)
    {
        if (!format) return;

        ModConsole::Sink* s = ResolveSink(sink, L"Printf");
        if (!s) return;

        char buf[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        buf[sizeof(buf) - 1] = '\0';

        s->Write(static_cast<ModConsole::LineKind>(static_cast<int>(kind)), buf);
    }

    static void ConsoleClear(PluginConsoleSink sink)
    {
        ModConsole::Sink* s = ResolveSink(sink, L"Clear");
        if (s) s->Clear();
    }

    static bool ConsoleExecute(const IPluginSelf* self, const char* line,
                               PluginConsoleOutputCallback onLine,
                               PluginConsoleCompleteCallback onComplete,
                               void* userData)
    {
        if (!self || !self->name || !line || !*line) return false;

        auto sink = std::make_shared<CallbackSink>(self->name, onLine, userData);

        std::string owner = self->name;
        std::function<void()> done;
        if (onComplete)
        {
            done = [onComplete, userData, owner]()
            {
                if (!StillMapped(reinterpret_cast<const void*>(onComplete)))
                {
                    ModLoaderLogger::LogWarn(L"[PluginConsole] Dropping completion for '%S': callback is unmapped",
                                             owner.c_str());
                    return;
                }
                onComplete(userData);
            };
        }

        return ModConsole::Dispatch(line, sink, done);
    }

    static IPluginConsole g_console = {
        ConsoleRegisterCommand,
        ConsoleUnregisterCommand,
        ConsoleUnregisterAllCommands,
        ConsoleHasCommand,
        ConsoleWrite,
        ConsolePrintf,
        ConsoleClear,
        ConsoleExecute,
    };

    IPluginConsole* GetInterface()
    {
        return &g_console;
    }

    void ForgetPlugin(const char* pluginName)
    {
        if (!pluginName || !*pluginName) return;

        const int removed = ModConsole::ForgetPluginCommands(pluginName);
        KillContexts(pluginName, nullptr);

        if (removed > 0)
            ModLoaderLogger::LogInfo(L"[PluginConsole] Removed %d console command(s) registered by '%S'",
                                     removed, pluginName);
    }
}
