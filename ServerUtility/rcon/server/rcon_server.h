#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

// Source Engine RCON protocol server (TCP).
//
// Packet layout (little-endian, on the wire):
//   int32  size  – byte count of the remaining packet (id + type + body + 2 nulls)
//   int32  id    – request ID echoed back in the response
//   int32  type  – packet type (see TYPE_* constants)
//   char[] body  – null-terminated command or response string
//   char   pad   – always 0x00
//
// Requires a password supplied via -RconPassword=.
// If no password is configured the server will refuse all incoming connections.
class RconServer
{
public:
    RconServer();
    ~RconServer();

    // Start listening on the given port with the given password.
    // Returns false if the socket cannot be bound.
    bool Start(uint16_t port, const std::string& password);

    void Stop();

    bool IsRunning() const { return m_running.load(); }

private:
    void ListenLoop();
    void HandleClient(SOCKET clientSock);
    bool RecvPacket(SOCKET s, int32_t& outId, int32_t& outType, std::string& outBody);
    bool SendPacket(SOCKET s, int32_t id, int32_t type, const std::string& body);

    // Track connected client sockets so Stop() can close them.
    void RegisterClient(SOCKET s);
    void UnregisterClient(SOCKET s);
    void CloseAllClients();

    SOCKET            m_listenSocket = INVALID_SOCKET;
    std::thread       m_listenThread;
    std::atomic<bool> m_running{ false };
    std::string       m_password;

    // Active client sockets (protected by m_clientsMutex)
    std::mutex       m_clientsMutex;
    std::vector<SOCKET> m_clientSockets;

    // Source Engine RCON packet types
    static constexpr int32_t TYPE_RESPONSE_VALUE = 0; // server -> client: command result
    static constexpr int32_t TYPE_AUTH_RESPONSE  = 2; // server -> client: auth result
    static constexpr int32_t TYPE_EXECCOMMAND    = 2; // client -> server: run command
    static constexpr int32_t TYPE_AUTH           = 3; // client -> server: authenticate
};
