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

    LOG_INFO("[RCON] Initiating shutdown...");

    // Close all connected client sockets first — this unblocks any threads
    // sitting in recv() so they can exit cleanly.
    CloseAllClients();

    // Close the listen socket to unblock accept()
    if (m_listenSocket != INVALID_SOCKET)
    {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    if (m_listenThread.joinable())
        m_listenThread.join();

    // Wait briefly for detached client handler threads to finish their
    // cleanup after their sockets were force-closed.  They only need to
    // run UnregisterClient + closesocket + LOG_INFO, which is sub-millisecond.
    Sleep(100);

    LOG_INFO("[RCON] Server stopped");
}

// ---------------------------------------------------------------------------
// Client socket tracking
// ---------------------------------------------------------------------------
void RconServer::RegisterClient(SOCKET s)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clientSockets.push_back(s);
}

void RconServer::UnregisterClient(SOCKET s)
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clientSockets.erase(
        std::remove(m_clientSockets.begin(), m_clientSockets.end(), s),
        m_clientSockets.end());
}

void RconServer::CloseAllClients()
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);
  for (SOCKET s : m_clientSockets)
 {
        // Shut down both directions — this causes any blocking recv() on
        // the client handler thread to return immediately with an error,
   // and also sends a TCP FIN to the remote client so it knows the
      // connection is being closed.
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    LOG_INFO("[RCON] Closed %zu client connection(s)", m_clientSockets.size());
    m_clientSockets.clear();
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
    // Register so Stop() can force-close this socket
    RegisterClient(clientSock);

    // 5-minute receive timeout — only catches completely stalled connections.
    // Interactive admin sessions may be idle for several minutes between commands,
    // so the old 30s value was far too aggressive.
    DWORD timeout = 300000;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));

 // TCP keepalives — detect dead peers (crashed client, dropped NAT entry, etc.)
    // at the socket layer without involving the RCON protocol at all.
    // Clients never see these probes, so no compatibility risk.
  int keepAlive = 1;
    setsockopt(clientSock, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
    // Start probing after 60 s of silence, then every 10 s, give up after 3 misses.
    DWORD keepIdle  = 60000;
    DWORD keepIntvl = 10000;
  DWORD keepCnt   = 3;
    setsockopt(clientSock, IPPROTO_TCP, TCP_KEEPIDLE,
         reinterpret_cast<const char*>(&keepIdle),  sizeof(keepIdle));
    setsockopt(clientSock, IPPROTO_TCP, TCP_KEEPINTVL,
               reinterpret_cast<const char*>(&keepIntvl), sizeof(keepIntvl));
    setsockopt(clientSock, IPPROTO_TCP, TCP_KEEPCNT,
           reinterpret_cast<const char*>(&keepCnt), sizeof(keepCnt));

    bool authenticated = false;

    while (m_running)
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

    // Unregister and clean up — if Stop() already closed this socket via
    // CloseAllClients(), closesocket() will harmlessly return WSAENOTSOCK.
    UnregisterClient(clientSock);
    shutdown(clientSock, SD_BOTH);
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
