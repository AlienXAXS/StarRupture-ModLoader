#pragma once

#include <string>
#include <vector>
#include <functional>
#include <initializer_list>

using CommandFunc = std::function<std::string(const std::string& args)>;

struct CommandRegistration
{
    std::vector<std::string> aliases;
    std::string              description;
    CommandFunc              handler;
    bool                     gameThread;  // true = dispatch to game thread via GameThreadDispatch
};

// Command registry with alias support.
// Commands are matched case-insensitively against all registered aliases.
class CommandHandler
{
public:
    static CommandHandler& Get();

    // Register a command with one or more aliases (first alias shown in help).
    // gameThread: if true (default), the handler is automatically dispatched to
    // the game thread.  Set to false only for commands that are safe to run on
    // any thread and don't touch engine state.
    void Register(std::initializer_list<std::string> aliases,
                  std::string description,
                  CommandFunc handler,
                  bool gameThread = true);

    // Execute a command line; splits on first space into verb + args.
    // If the matched command was registered with gameThread=true, the handler
    // is posted to GameThreadDispatch and the calling thread blocks until it
    // completes (30 s timeout).
    std::string Execute(const std::string& cmdLine) const;

    // Return a formatted help string listing all commands and aliases
    std::string GetHelp() const;

private:
    std::vector<CommandRegistration> m_commands;
};
