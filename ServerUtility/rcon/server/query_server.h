#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

// Steam Game Server Query Protocol server (UDP).
//
// Implements the A2S (Any-to-Server) query protocol used by the Steam server
// browser and compatible tools.  Runs on the same port number as RCON but
// uses UDP so there is no socket conflict.
//
// Protocols handled:
//   A2S_INFO   (0x54) – server information (name, world, players, etc.)
//   A2S_PLAYER (0x55) – player list with a challenge-response handshake
//   A2S_RULES  (0x56) – key-value server rules
class QueryServer
{
public:
    QueryServer();
    ~QueryServer();

    bool Start(uint16_t port);
    void Stop();

    bool IsRunning() const { return m_running.load(); }

private:
    void ReceiveLoop();
    void HandlePacket(const uint8_t* data, int len, const sockaddr_in& from);

    void HandleA2sInfo  (const sockaddr_in& from);
    void HandleA2sPlayer(const sockaddr_in& from, const uint8_t* data, int len);
    void HandleA2sRules (const sockaddr_in& from);

    void Send(const sockaddr_in& to, const std::vector<uint8_t>& data);

    static std::vector<uint8_t> BuildA2sInfoResponse();
    static std::vector<uint8_t> BuildA2sPlayerChallenge(uint32_t challenge);
    static std::vector<uint8_t> BuildA2sPlayerResponse();
    static std::vector<uint8_t> BuildA2sRulesResponse();

    SOCKET            m_socket  = INVALID_SOCKET;
    std::thread       m_thread;
    std::atomic<bool> m_running{ false };

    // Per-client challenge tracking (IP+port → challenge number)
    uint32_t                            m_nextChallenge = 0x12345678u;
    std::unordered_map<uint64_t, uint32_t> m_challenges;
};
