#pragma comment(lib, "ws2_32.lib")

#include "rcon_server.h"
#include "plugin_helpers.h"
#include "../commands/command_handler.h"

#include <ws2tcpip.h>
#include <vector>
#include <algorithm>
#include <cstring>

RconServer::RconServer()  = default;
RconServer::~RconServer() { Stop(); }

bool RconServer::Start(uint16_t port, const std::string& password)
{
    if (password.empty())
    {
        LOG_WARN("[RCON] No -RconPassword= set – RCON is disabled.");
        return false;
    }

    m_password = password;

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET)
    {
        LOG_ERROR("[RCON] socket() failed: %d", WSAGetLastError());
        return false;
    }

    // Allow fast port reuse after restart
    int opt = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        LOG_ERROR("[RCON] bind() failed on port %d: %d", port, WSAGetLastError());
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        LOG_ERROR("[RCON] listen() failed: %d", WSAGetLastError());
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_listenThread = std::thread(&RconServer::ListenLoop, this);

    LOG_INFO("[RCON] TCP RCON listening on port %d", port);
    return true;
}

void RconServer::Stop()
{
    if (!m_running.exchange(false))
        return;

    if (m_listenSocket != INVALID_SOCKET)
    {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    if (m_listenThread.joinable())
        m_listenThread.join();

    LOG_INFO("[RCON] Server stopped");
}

void RconServer::ListenLoop()
{
    while (m_running)
    {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);

        SOCKET clientSock = accept(m_listenSocket,
                                   reinterpret_cast<sockaddr*>(&clientAddr),
                                   &addrLen);
        if (clientSock == INVALID_SOCKET)
        {
            if (m_running)
                LOG_WARN("[RCON] accept() failed: %d", WSAGetLastError());
            break;
        }

        char ipBuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
        LOG_INFO("[RCON] Client connected from %s", ipBuf);

        // Detach a thread per client (dedicated server, low concurrency expected)
        std::thread([this, clientSock]() { HandleClient(clientSock); }).detach();
    }
}

void RconServer::HandleClient(SOCKET clientSock)
{
    // 30-second receive timeout — disconnects idle / stalled clients
    DWORD timeout = 30000;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    bool authenticated = false;

    for (;;)
    {
        int32_t     id, type;
        std::string body;

        if (!RecvPacket(clientSock, id, type, body))
            break;

        if (!authenticated)
        {
            if (type == TYPE_AUTH)
            {
                if (body == m_password)
                {
                    authenticated = true;
                    // Source RCON spec: send empty RESPONSE_VALUE first, then AUTH_RESPONSE.
                    // Many clients block waiting for RESPONSE_VALUE and will never advance
                    // past auth without it — causing the 30-second SO_RCVTIMEO timeout loop.
                    SendPacket(clientSock, id, TYPE_RESPONSE_VALUE, "");
                    SendPacket(clientSock, id, TYPE_AUTH_RESPONSE, "");
                    LOG_INFO("[RCON] Client authenticated successfully");
                }
                else
                {
                    // Source RCON spec: id=-1 means auth failure
                    SendPacket(clientSock, -1, TYPE_AUTH_RESPONSE, "");
                    LOG_WARN("[RCON] Client failed authentication (wrong password)");
                    break;
                }
            }
            else
            {
                // Unauthenticated client sent a non-auth packet — disconnect
                break;
            }
        }
        else
        {
            if (type == TYPE_EXECCOMMAND)
            {
                std::string response = CommandHandler::Get().Execute(body);
                SendPacket(clientSock, id, TYPE_RESPONSE_VALUE, response);
            }
            else if (type == TYPE_AUTH)
            {
                // Re-auth attempt (some RCON clients do this periodically)
                if (body == m_password)
                    SendPacket(clientSock, id, TYPE_AUTH_RESPONSE, "");
                else
                    SendPacket(clientSock, -1, TYPE_AUTH_RESPONSE, "");
            }
        }
    }

    closesocket(clientSock);
    LOG_INFO("[RCON] Client disconnected");
}

// Accumulate exactly 'needed' bytes, tolerating short reads.
// MSG_WAITALL on Windows can return short under SO_RCVTIMEO, causing silent
// data corruption; this loop is always correct.
static bool RecvAll(SOCKET s, char* buf, int needed)
{
    int got = 0;
    while (got < needed)
    {
        int n = recv(s, buf + got, needed - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool RconServer::RecvPacket(SOCKET s, int32_t& outId, int32_t& outType, std::string& outBody)
{
    // Read the 4-byte little-endian packet size
    int32_t size = 0;
    if (!RecvAll(s, reinterpret_cast<char*>(&size), sizeof(size)) || size < 8)
        return false;

    // Guard against unreasonably large packets (Source RCON max is 4096 bytes)
    if (size > 4096)
        return false;

    std::vector<char> buf(static_cast<size_t>(size));
    if (!RecvAll(s, buf.data(), size))
        return false;

    memcpy(&outId,   buf.data(),     sizeof(int32_t));
    memcpy(&outType, buf.data() + 4, sizeof(int32_t));

    // Body starts at offset 8 and is null-terminated
    outBody = std::string(buf.data() + 8);
    return true;
}

bool RconServer::SendPacket(SOCKET s, int32_t id, int32_t type, const std::string& body)
{
    // size = id(4) + type(4) + body + null + trailing null
    const int32_t bodyLen = static_cast<int32_t>(body.size());
    const int32_t size    = 4 + 4 + bodyLen + 1 + 1;

    std::vector<char> pkt(static_cast<size_t>(4 + size));
    int offset = 0;

    memcpy(pkt.data() + offset, &size, 4); offset += 4;
    memcpy(pkt.data() + offset, &id,   4); offset += 4;
    memcpy(pkt.data() + offset, &type, 4); offset += 4;

    if (bodyLen > 0)
    {
        memcpy(pkt.data() + offset, body.data(), static_cast<size_t>(bodyLen));
        offset += bodyLen;
    }

    pkt[offset++] = '\0'; // body null terminator
    pkt[offset++] = '\0'; // trailing pad

    const int totalLen = static_cast<int>(pkt.size());
    return send(s, pkt.data(), totalLen, 0) == totalLen;
}
