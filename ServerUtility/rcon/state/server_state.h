#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct PlayerInfo
{
    std::string name;
    uint32_t    pingMs;   // APlayerState::CompressedPing (direct ms value, UE5)
    float       duration; // seconds connected (0 if unavailable)
    std::string ipAddress; // player's IP address (empty if unavailable)
};

// Thread-safe game state cache.
// Written from the game thread (via plugin callbacks), read from RCON / Query threads.
class ServerState
{
public:
    static ServerState& Get();

    // ----- Setters (game thread) -----
    void SetServerName(const std::string& name);
    void SetWorldName (const std::string& name);
    void SetMaxPlayers(int max);
    void UpdatePlayers(std::vector<PlayerInfo> players);

    // ----- Getters (any thread, return by value) -----
    std::string             GetServerName()  const;
    std::string             GetWorldName()   const;
    int                     GetMaxPlayers()  const;
    int                     GetPlayerCount() const;
    std::vector<PlayerInfo> GetPlayers()     const;

private:
    mutable std::mutex      m_mutex;
    std::string             m_serverName = "StarRupture Server";
    std::string             m_worldName  = "Unknown";
    int                     m_maxPlayers = 4;
    std::vector<PlayerInfo> m_players;
};
