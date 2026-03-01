#pragma comment(lib, "ws2_32.lib")

#include "query_server.h"
#include "plugin_helpers.h"
#include "../state/server_state.h"

#include <ws2tcpip.h>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Steam A2S protocol constants
// ---------------------------------------------------------------------------
static constexpr uint8_t A2S_INFO_HEADER   = 0x54; // 'T' – client info request
static constexpr uint8_t S2A_INFO_HEADER   = 0x49; // 'I' – server info response
static constexpr uint8_t A2S_PLAYER_HEADER = 0x55; // 'U' – client player request
static constexpr uint8_t S2A_PLAYER_HEADER = 0x44; // 'D' – server player response
static constexpr uint8_t S2C_CHALLENGE     = 0x41; // 'A' – server challenge response
static constexpr uint8_t A2S_RULES_HEADER  = 0x56; // 'V' – client rules request
static constexpr uint8_t S2A_RULES_HEADER  = 0x45; // 'E' – server rules response

static constexpr uint8_t PROTOCOL_VERSION  = 17;

// ---------------------------------------------------------------------------
// Buffer helpers
// ---------------------------------------------------------------------------
static void PushString(std::vector<uint8_t>& buf, const std::string& s)
{
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}

template<typename T>
static void PushLE(std::vector<uint8_t>& buf, T val)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
    buf.insert(buf.end(), p, p + sizeof(T));
}

// Unique key combining IP and port for per-client challenge tracking
static uint64_t AddressKey(const sockaddr_in& addr)
{
    return (static_cast<uint64_t>(addr.sin_addr.s_addr) << 16) |
            static_cast<uint64_t>(ntohs(addr.sin_port));
}

// ---------------------------------------------------------------------------
QueryServer::QueryServer()  = default;
QueryServer::~QueryServer() { Stop(); }

bool QueryServer::Start(uint16_t port)
{
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET)
    {
        LOG_ERROR("[Query] socket() failed: %d", WSAGetLastError());
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        LOG_ERROR("[Query] bind() failed on port %d: %d", port, WSAGetLastError());
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_thread  = std::thread(&QueryServer::ReceiveLoop, this);

    LOG_INFO("[Query] UDP A2S query server listening on port %d", port);
    return true;
}

void QueryServer::Stop()
{
    if (!m_running.exchange(false))
        return;

    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_thread.joinable())
        m_thread.join();

    LOG_INFO("[Query] Server stopped");
}

void QueryServer::ReceiveLoop()
{
    uint8_t buf[1400]; // Max UDP payload to stay within MTU
    while (m_running)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);

        int bytes = recvfrom(m_socket,
                             reinterpret_cast<char*>(buf), sizeof(buf),
                             0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (bytes <= 0)
        {
            if (m_running)
                LOG_WARN("[Query] recvfrom() failed: %d", WSAGetLastError());
            break;
        }

        HandlePacket(buf, bytes, from);
    }
}

void QueryServer::HandlePacket(const uint8_t* data, int len, const sockaddr_in& from)
{
    // All A2S packets begin with the 0xFFFFFFFF header followed by a type byte
    if (len < 5 ||
        data[0] != 0xFF || data[1] != 0xFF ||
        data[2] != 0xFF || data[3] != 0xFF)
        return;

    const uint8_t header = data[4];

    if (header == A2S_INFO_HEADER)
        HandleA2sInfo(from);
    else if (header == A2S_PLAYER_HEADER)
        HandleA2sPlayer(from, data, len);
    else if (header == A2S_RULES_HEADER)
        HandleA2sRules(from);
}

void QueryServer::HandleA2sInfo(const sockaddr_in& from)
{
    Send(from, BuildA2sInfoResponse());
}

void QueryServer::HandleA2sPlayer(const sockaddr_in& from, const uint8_t* data, int len)
{
    if (len < 9) return; // need header(4) + type(1) + challenge(4)

    uint32_t requestChallenge = 0;
    memcpy(&requestChallenge, data + 5, sizeof(uint32_t));

    const uint64_t key = AddressKey(from);

    if (requestChallenge == 0xFFFFFFFFu)
    {
        // Client is requesting a challenge — generate one and send it back
        const uint32_t challenge = m_nextChallenge++;
        m_challenges[key] = challenge;
        Send(from, BuildA2sPlayerChallenge(challenge));
        return;
    }

    // Validate the challenge
    auto it = m_challenges.find(key);
    if (it == m_challenges.end() || it->second != requestChallenge)
    {
        // Unknown or stale challenge — issue a fresh one
        const uint32_t challenge = m_nextChallenge++;
        m_challenges[key] = challenge;
        Send(from, BuildA2sPlayerChallenge(challenge));
        return;
    }

    // Valid challenge: consume it and respond with the player list
    m_challenges.erase(it);
    Send(from, BuildA2sPlayerResponse());
}

void QueryServer::HandleA2sRules(const sockaddr_in& from)
{
    Send(from, BuildA2sRulesResponse());
}

void QueryServer::Send(const sockaddr_in& to, const std::vector<uint8_t>& data)
{
    sendto(m_socket,
           reinterpret_cast<const char*>(data.data()),
           static_cast<int>(data.size()),
           0,
           reinterpret_cast<const sockaddr*>(&to),
           sizeof(to));
}

// ---------------------------------------------------------------------------
// Response builders
// ---------------------------------------------------------------------------

std::vector<uint8_t> QueryServer::BuildA2sInfoResponse()
{
    auto& state = ServerState::Get();

    std::vector<uint8_t> buf;
    buf.reserve(256);

    // Packet header
    PushLE<uint32_t>(buf, 0xFFFFFFFFu);
    buf.push_back(S2A_INFO_HEADER);

    // Protocol version
    buf.push_back(PROTOCOL_VERSION);

    // Server name
    PushString(buf, state.GetServerName());

    // Map (world name)
    PushString(buf, state.GetWorldName());

    // Folder (game directory name)
    PushString(buf, "StarRupture");

    // Game description
    PushString(buf, "Star Rupture");

    // Steam App ID (0 = no app ID)
    PushLE<uint16_t>(buf, 0);

    // Current / max player counts
    buf.push_back(static_cast<uint8_t>(std::min(state.GetPlayerCount(), 255)));
    buf.push_back(static_cast<uint8_t>(std::min(state.GetMaxPlayers(), 255)));

    // Bots
    buf.push_back(0);

    // Server type: 'd' = dedicated
    buf.push_back('d');

    // OS: 'w' = Windows
    buf.push_back('w');

    // Visibility: 0 = public
    buf.push_back(0);

    // VAC: 0 = unsecured
    buf.push_back(0);

    // Game version
    PushString(buf, "1.0.0.0");

    // Extra data flags (none)
    buf.push_back(0x00);

    return buf;
}

std::vector<uint8_t> QueryServer::BuildA2sPlayerChallenge(uint32_t challenge)
{
    std::vector<uint8_t> buf;
    PushLE<uint32_t>(buf, 0xFFFFFFFFu);
    buf.push_back(S2C_CHALLENGE);
    PushLE<uint32_t>(buf, challenge);
    return buf;
}

std::vector<uint8_t> QueryServer::BuildA2sPlayerResponse()
{
    const auto players = ServerState::Get().GetPlayers();

    std::vector<uint8_t> buf;
    buf.reserve(64 + players.size() * 32);

    PushLE<uint32_t>(buf, 0xFFFFFFFFu);
    buf.push_back(S2A_PLAYER_HEADER);

    const uint8_t count = static_cast<uint8_t>(std::min(players.size(), size_t(255)));
    buf.push_back(count);

    for (uint8_t i = 0; i < count; ++i)
    {
        buf.push_back(i);                          // index (0-based)
        PushString(buf, players[i].name);           // player name
        PushLE<int32_t>(buf, 0);                   // score (0; not tracked)
        PushLE<float>(buf, players[i].duration);   // seconds connected
    }

    return buf;
}

std::vector<uint8_t> QueryServer::BuildA2sRulesResponse()
{
    auto& state = ServerState::Get();

    const std::vector<std::pair<std::string, std::string>> rules = {
        { "world",      state.GetWorldName()                          },
        { "players",    std::to_string(state.GetPlayerCount())        },
        { "maxplayers", std::to_string(state.GetMaxPlayers())         },
    };

    std::vector<uint8_t> buf;
    PushLE<uint32_t>(buf, 0xFFFFFFFFu);
    buf.push_back(S2A_RULES_HEADER);
    PushLE<uint16_t>(buf, static_cast<uint16_t>(rules.size()));

    for (const auto& [k, v] : rules)
    {
        PushString(buf, k);
        PushString(buf, v);
    }

    return buf;
}
