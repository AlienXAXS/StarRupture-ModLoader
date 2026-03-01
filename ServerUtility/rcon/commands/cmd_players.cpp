#include "cmd_players.h"
#include "command_handler.h"
#include "../state/server_state.h"

#include <sstream>
#include <iomanip>
#include <cmath>

namespace Cmd_Players
{
    // Format seconds into a human-readable duration string (e.g. "2h 15m 30s")
    static std::string FormatDuration(float seconds)
    {
        if (seconds <= 0.0f)
            return "N/A";

        int totalSec = static_cast<int>(std::floor(seconds));
        int hours   = totalSec / 3600;
        int minutes = (totalSec % 3600) / 60;
        int secs    = totalSec % 60;

        std::ostringstream oss;
        if (hours > 0)
            oss << hours << "h ";
        if (hours > 0 || minutes > 0)
            oss << minutes << "m ";
        oss << secs << "s";

        return oss.str();
    }

    static std::string Handle(const std::string& /*args*/)
    {
        const auto players = ServerState::Get().GetPlayers();

        if (players.empty())
            return "No players currently connected.\n";

        std::ostringstream oss;
        oss << "Players (" << players.size() << " connected):\n";
        oss << "  " << std::left
            << std::setw(24) << "Player Name"
            << std::setw(16) << "Time On Server"
            << std::setw(18) << "IP Address"
            << "Latency\n";
        oss << "  " << std::string(70, '-') << "\n";

        int idx = 1;
        for (const auto& p : players)
        {
            std::string indexedName = "[" + std::to_string(idx++) + "] " + p.name;
            std::string duration    = FormatDuration(p.duration);
            std::string ip          = p.ipAddress.empty() ? "N/A" : p.ipAddress;
            std::string latency     = std::to_string(p.pingMs) + " ms";

            oss << "  " << std::left
                << std::setw(24) << indexedName
                << std::setw(16) << duration
                << std::setw(18) << ip
                << latency << "\n";
        }

        return oss.str();
    }

    void Register(CommandHandler& handler)
    {
        handler.Register(
            { "players", "list", "who" },
            "List all connected players with their ping",
            Handle);
    }
}
