#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ModConsole -- the mod loader's own console command registry
//
// One registry, two front-ends: the ImGui developer console on client builds
// (UI/console_window.cpp) and the Win32 console window on any build started
// with -console (console/server_console.cpp). Both parse a line, hand it to
// Dispatch(), and print whatever comes back through their own Sink -- so a
// command added here is immediately available in both, and `help` lists the
// same set wherever it is typed.
//
// These are mod loader commands, not engine commands. The client console tries
// this registry first and falls through to APlayerController::ConsoleCommand
// when the first token is not one of ours, so engine commands keep working
// exactly as before.
//
// Threading: a command declaring gameThread == true is posted to the game
// thread by Dispatch and runs during the next engine tick. Everything a plugin
// does in PluginInit/PluginShutdown touches engine state, so plugin
// load/unload/reload must not run on a console reader thread or the render
// thread. That is why Dispatch is asynchronous: neither front-end can afford
// to block waiting for a tick.
// ---------------------------------------------------------------------------

namespace ModConsole
{
    enum class LineKind
    {
        Output,   // normal command output
        Notice,   // dimmed supporting text (headers, hints)
        Error     // something went wrong
    };

    // Where a command's output goes. Each console front-end implements Write
    // for its own display.
    //
    // A Sink is held by shared_ptr for the whole life of a dispatched command,
    // because a game-thread command outlives the call to Dispatch that started
    // it. Write may therefore be called from the game thread rather than the
    // thread that typed the command -- implementations must be thread-safe.
    class Sink
    {
    public:
        virtual ~Sink() = default;

        virtual void Write(LineKind kind, const char* text) = 0;

        // Wipe the hosting console's scrollback. Default: ignore -- a console
        // with no scrollback to clear is a valid console.
        virtual void Clear() {}

        // printf-style helpers, in terms of Write.
        void Printf(LineKind kind, const char* fmt, ...);
        void Out(const char* fmt, ...);
        void Notice(const char* fmt, ...);
        void Error(const char* fmt, ...);
    };

    // args[0] is the command name as typed; the rest are its arguments.
    using Handler = void (*)(const std::vector<std::string>& args, Sink& out);

    // Same, plus an opaque context. Plugin-registered commands use this form
    // so one plugin entry point can back several commands.
    using ContextHandler = void (*)(const std::vector<std::string>& args, Sink& out, void* context);

    struct Command
    {
        const char* name;
        const char* aliases;    // space-separated, may be null
        const char* usage;      // e.g. "reload <plugin|index|all>"
        const char* help;       // one line, shown by `help`
        Handler     handler;
        bool        gameThread; // run on the game thread instead of the caller's

        // --- plugin-registered commands only; all null/zero for built-ins ---

        // Owning plugin name. The registry owns copies of every string above
        // for these entries, because the literals a plugin passes live in its
        // DLL and that DLL can be unloaded while the command is still listed.
        const char*    owner;

        // Called instead of handler when non-null.
        ContextHandler ctxHandler;
        void*          context;
    };

    // Register the mod loader's built-in commands. Idempotent; called by both
    // front-ends so neither has to depend on the other having started.
    void RegisterBuiltins();

    // Add a command. The strings must outlive the process (literals in
    // practice) -- built-ins pass literals from this DLL. Duplicate names are
    // ignored, which is what makes RegisterBuiltins idempotent.
    void Register(const Command& cmd);

    // Add a command on behalf of a plugin (owner = plugin name). Unlike
    // Register, every string in cmd is copied, cmd.ctxHandler/context are used
    // instead of cmd.handler, and the entry can be removed again.
    //
    // False when the name (or one of its aliases) is already taken -- a plugin
    // may not shadow a built-in or another plugin's command.
    bool RegisterPluginCommand(const char* owner, const Command& cmd);

    // Remove one of owner's commands by its registered name. False when that
    // name is not registered, or is registered to somebody else.
    bool UnregisterPluginCommand(const char* owner, const char* name);

    // Remove every command owned by owner; returns how many went. Called by
    // PluginManager before FreeLibrary -- a command left behind is a handler
    // pointer into an unmapped module.
    int ForgetPluginCommands(const char* owner);

    // A copy of one command's public description.
    //
    // Values, not pointers into the registry: a plugin command disappears the
    // moment its plugin is unloaded, and every caller outside this file is on
    // some other thread. Nothing that survives a lock can be a borrowed string.
    struct CommandInfo
    {
        std::string name;
        std::string aliases;
        std::string usage;
        std::string help;
        std::string owner;        // empty for built-ins
        bool        gameThread = false;
    };

    // True when name, or one of the aliases, resolves to a command.
    bool Exists(const char* name);

    // Look up by name or alias, case-insensitively. False when there is no match.
    bool FindInfo(const char* name, CommandInfo& out);

    // Every registered command, in registration order.
    std::vector<CommandInfo> GetCommands();

    // Split a command line into tokens, honouring "double quoted" runs so a
    // plugin name containing spaces can be passed as one argument.
    std::vector<std::string> Tokenize(const std::string& line);

    // Run a command line.
    //
    // Returns false without running anything when the first token is not a
    // registered command -- that is the front-end's cue to try the engine
    // console (client) or print an unknown-command message (server).
    //
    // Returns true once the command has been *started*: a gameThread command is
    // still queued at that point and its output arrives on the sink later.
    // onComplete (optional) fires on whichever thread ran the handler, after
    // the last line has been written.
    bool Dispatch(const std::string& line,
                  std::shared_ptr<Sink> sink,
                  std::function<void()> onComplete = {});

    // True while this sink is inside a handler Dispatch is running.
    //
    // Exists for the plugin-facing console interface: a plugin's handler is
    // handed its Sink as an opaque pointer, and writing through one after the
    // handler returned is a use-after-free of whatever the front-end owns. The
    // registry knows which sinks are live, so that mistake is answered with a
    // log line instead of a crash.
    bool IsSinkActive(const Sink* sink);
}
