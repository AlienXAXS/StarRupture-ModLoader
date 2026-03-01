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
};

// Command registry with alias support.
// Commands are matched case-insensitively against all registered aliases.
class CommandHandler
{
public:
    static CommandHandler& Get();

    // Register a command with one or more aliases (first alias shown in help)
    void Register(std::initializer_list<std::string> aliases,
                  std::string description,
                  CommandFunc handler);

    // Execute a command line; splits on first space into verb + args
    std::string Execute(const std::string& cmdLine) const;

    // Return a formatted help string listing all commands and aliases
    std::string GetHelp() const;

private:
    std::vector<CommandRegistration> m_commands;
};
