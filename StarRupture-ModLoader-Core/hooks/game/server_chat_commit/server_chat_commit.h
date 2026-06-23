#pragma once

#ifdef MODLOADER_SERVER_BUILD
namespace Hooks::ServerChatCommit
{
    // Install the ExecFunction hook on ACrPlayerControllerBase::ServerExecuteConsoleCommand.
    // Intercepts Client->Server mod envelopes (commands starting with "[MOD:") and
    // dispatches them to NetworkChannel::DispatchServerMessage.
    // Non-mod commands pass through to the real console command handler.
    // Called lazily by network_channel.cpp when the first server-message handler
    // is registered.  Safe to call multiple times (no-op if already installed).
    bool Install();

    // Remove the hook.  Called from NetworkChannel::Shutdown().
    void Remove();

    // Returns true if the hook is currently installed.
    bool IsInstalled();
}
#endif // MODLOADER_SERVER_BUILD
