#pragma once

class CommandHandler;

namespace Cmd_Save
{
    // Register the save command.
    // Aliases: save, savegame, forcesave
    void Register(CommandHandler& handler);
}
