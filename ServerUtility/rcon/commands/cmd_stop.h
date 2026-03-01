#pragma once

class CommandHandler;

namespace Cmd_Stop
{
    // Register the stop/shutdown command.
    // Aliases: stop, quit, exit, shutdown
    void Register(CommandHandler& handler);
}
