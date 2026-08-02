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

    struct Command
    {
        const char* name;
        const char* aliases;    // space-separated, may be null
        const char* usage;      // e.g. "reload <plugin|index|all>"
        const char* help;       // one line, shown by `help`
        Handler     handler;
        bool        gameThread; // run on the game thread instead of the caller's
    };

    // Register the mod loader's built-in commands. Idempotent; called by both
    // front-ends so neither has to depend on the other having started.
    void RegisterBuiltins();

    // Add a command. The strings must outlive the process (literals in
    // practice); the returned pointers from Find/GetCommands stay valid.
    void Register(const Command& cmd);

    // Look up by name or alias, case-insensitively. Null when there is no match.
    const Command* Find(const char* name);

    // Every registered command, in registration order.
    std::vector<const Command*> GetCommands();

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
}
