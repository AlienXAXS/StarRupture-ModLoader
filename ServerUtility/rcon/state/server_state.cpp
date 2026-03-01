#include "server_state.h"

ServerState& ServerState::Get()
{
    static ServerState instance;
    return instance;
}

void ServerState::SetServerName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_serverName = name;
}

void ServerState::SetWorldName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_worldName = name;
}

void ServerState::SetMaxPlayers(int max)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxPlayers = max;
}

void ServerState::UpdatePlayers(std::vector<PlayerInfo> players)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_players = std::move(players);
}

std::string ServerState::GetServerName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_serverName;
}

std::string ServerState::GetWorldName() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_worldName;
}

int ServerState::GetMaxPlayers() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxPlayers;
}

int ServerState::GetPlayerCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_players.size());
}

std::vector<PlayerInfo> ServerState::GetPlayers() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_players;
}
