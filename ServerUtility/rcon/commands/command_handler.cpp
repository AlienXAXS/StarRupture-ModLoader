#include "command_handler.h"

#include <sstream>
#include <algorithm>

CommandHandler& CommandHandler::Get()
{
    static CommandHandler instance;
    return instance;
}

void CommandHandler::Register(std::initializer_list<std::string> aliases,
                               std::string description,
                               CommandFunc handler)
{
    CommandRegistration reg;
    reg.aliases     = aliases;
    reg.description = std::move(description);
    reg.handler     = std::move(handler);
    m_commands.push_back(std::move(reg));
}

std::string CommandHandler::Execute(const std::string& cmdLine) const
{
    if (cmdLine.empty())
        return GetHelp();

    // Split into verb and optional args on the first space
    std::string verb, args;
    size_t sp = cmdLine.find(' ');
    if (sp == std::string::npos)
    {
        verb = cmdLine;
    }
    else
    {
        verb = cmdLine.substr(0, sp);
        // Trim leading spaces from args
        size_t argsStart = cmdLine.find_first_not_of(' ', sp);
        if (argsStart != std::string::npos)
            args = cmdLine.substr(argsStart);
    }

    // Normalise verb to lowercase for case-insensitive matching
    std::string verbLower = verb;
    std::transform(verbLower.begin(), verbLower.end(), verbLower.begin(), ::tolower);

    for (const auto& reg : m_commands)
    {
        for (const auto& alias : reg.aliases)
        {
            std::string aliasLower = alias;
            std::transform(aliasLower.begin(), aliasLower.end(), aliasLower.begin(), ::tolower);
            if (aliasLower == verbLower)
                return reg.handler(args);
        }
    }

    return "Unknown command: \"" + verb + "\"\n\n" + GetHelp();
}

std::string CommandHandler::GetHelp() const
{
    std::ostringstream oss;
    oss << "Available commands:\n";
    for (const auto& reg : m_commands)
    {
        oss << "  ";
        for (size_t i = 0; i < reg.aliases.size(); ++i)
        {
            if (i > 0) oss << " | ";
            oss << reg.aliases[i];
        }
        oss << "\n      " << reg.description << "\n";
    }
    return oss.str();
}
