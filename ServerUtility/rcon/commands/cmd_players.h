#pragma once

class CommandHandler;

namespace Cmd_Players
{
    // Register the players command.
    // Aliases: players, list, who
    void Register(CommandHandler& handler);
}
