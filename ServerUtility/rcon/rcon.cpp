// winsock2.h must be included before windows.h to avoid winsock.h conflicts
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include "rcon.h"
#include "plugin_helpers.h"

#include "state/server_state.h"
#include "state/game_thread_dispatch.h"
#include "server/rcon_server.h"
#include "server/query_server.h"
#include "commands/command_handler.h"
#include "commands/cmd_players.h"
#include "commands/cmd_stop.h"
#include "commands/cmd_save.h"

// SDK access: only compile player-collection code when we have the engine SDK.
// Server/Client configs get MODLOADER_SERVER_BUILD / MODLOADER_CLIENT_BUILD via
// Shared.props and include the full SDK headers.
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
// Targeted include: brings in UWorld, AGameStateBase, APlayerState without
// pulling in the entire SDK.  Engine_classes.hpp includes Basic.hpp itself
// which sets up the SDK namespace and UC container aliases.
#  include "SDK/Engine_classes.hpp"
#  define RCON_HAS_SDK 1
#else
#  define RCON_HAS_SDK 0
#endif

#include <string>
#include <thread>
#include <atomic>
#include <cwchar>
#include <cstdlib>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static RconServer  g_rconServer;
static QueryServer g_queryServer;

static std::thread       g_refreshThread;
static std::atomic<bool> g_refreshRunning{ false };

// Current world pointer — written from the game thread, read (under SEH) from
// the refresh thread.  Plain pointer; no ref-counting needed since we clear it
// in OnEngineShutdown before the world is destroyed.
static std::atomic<void*> g_currentWorld{ nullptr };

// ---------------------------------------------------------------------------
// Command-line helpers (wchar_t variant, mirrors parse_settings.cpp)
// ---------------------------------------------------------------------------
static bool GetCmdParam(const wchar_t* paramName, std::wstring& out)
{
	const wchar_t* cmdLine = GetCommandLineW();
	if (!cmdLine) return false;

	const wchar_t* pos = wcsstr(cmdLine, paramName);
	if (!pos) return false;

	pos += wcslen(paramName);

	bool quoted = (*pos == L'"');
	if (quoted) ++pos;

	const wchar_t* end = pos;
	if (quoted)
	{
		while (*end && *end != L'"') ++end;
	}
	else
	{
		while (*end && *end != L' ' && *end != L'\t') ++end;
	}

	out.assign(pos, end);
	return !out.empty();
}

static uint16_t ReadRconPort()
{
	std::wstring val;
	if (GetCmdParam(L"-RconPort=", val))
	{
		const int port = _wtoi(val.c_str());
		if (port > 0 && port <= 65535)
			return static_cast<uint16_t>(port);
	}
	return 0; // Not specified
}

static std::string ReadRconPassword()
{
	std::wstring val;
	if (!GetCmdParam(L"-RconPassword=", val) || val.empty())
		return {};

	// Convert wide string to UTF-8
	const int len = WideCharToMultiByte(CP_UTF8, 0,
		val.c_str(), static_cast<int>(val.size()),
		nullptr, 0, nullptr, nullptr);
	if (len <= 0) return {};

	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0,
		val.c_str(), static_cast<int>(val.size()),
		out.data(), len, nullptr, nullptr);
	return out;
}

static std::string ReadSessionName()
{
	std::wstring val;
	if (!GetCmdParam(L"-SessionName=", val) || val.empty())
		return "StarRupture Server";

	const int len = WideCharToMultiByte(CP_UTF8, 0,
		val.c_str(), static_cast<int>(val.size()),
		nullptr, 0, nullptr, nullptr);
	if (len <= 0) return "StarRupture Server";

	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0,
		val.c_str(), static_cast<int>(val.size()),
		out.data(), len, nullptr, nullptr);
	return out;
}

// ---------------------------------------------------------------------------
// Player state collection (runs on the refresh thread)
// ---------------------------------------------------------------------------

// Inner SEH filter that catches access violations when reading stale engine
// pointers.  This function must NOT contain any C++ objects with destructors
// (MSVC restriction: C2712).  We only read raw pointers here and pass them
// out so the caller (a normal C++ function) can build the PlayerInfo vector.
#if RCON_HAS_SDK
struct RawPlayerData
{
	const char* namePtr;   // points into FString internal buffer (transient!)
	int   nameLen;
	uint8_t     ping;
};

static constexpr int MAX_RAW_PLAYERS = 128;

// Returns the number of players written into outRaw, or -1 on SEH exception.
static int CollectPlayersRaw_SEH(void* worldPtr, RawPlayerData* outRaw)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(worldPtr);
		if (!world) return 0;

		SDK::AGameStateBase* gameState = world->GameState;
		if (!gameState) return 0;

		const int32_t count = gameState->PlayerArray.Num();
		int written = 0;

		for (int32_t i = 0; i < count && written < MAX_RAW_PLAYERS; ++i)
		{
			SDK::APlayerState* ps = gameState->PlayerArray[i];
			if (!ps) continue;

			if (ps->PlayerNamePrivate.Num() <= 0)
				continue;

			// Grab the raw TCHAR* – this is a wchar_t* internally.
					 // We'll do the conversion outside __try.
			outRaw[written].namePtr = nullptr;
			outRaw[written].nameLen = ps->PlayerNamePrivate.Num();
			outRaw[written].ping = ps->CompressedPing;

			// The SDK FString stores wchar_t internally; ToString() returns
		   // std::string, but that involves a destructor. Instead store the
		  // raw data ptr and length so we can convert safely outside SEH.
			   // FString's Data() returns the internal wchar_t buffer pointer.
			outRaw[written].namePtr = reinterpret_cast<const char*>(ps->PlayerNamePrivate.CStr());
			++written;
		}

		return written;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LOG_WARN("[Rcon] Exception while collecting player state (0x%08lX) – skipping",
			GetExceptionCode());
		return -1;
	}
}
#endif

static void CollectPlayers(void* worldPtr)
{
	if (!worldPtr) return;

#if RCON_HAS_SDK
	RawPlayerData rawBuf[MAX_RAW_PLAYERS];
	int rawCount = CollectPlayersRaw_SEH(worldPtr, rawBuf);
	if (rawCount <= 0) return;

	std::vector<PlayerInfo> players;
	players.reserve(static_cast<size_t>(rawCount));

	for (int i = 0; i < rawCount; ++i)
	{
		if (!rawBuf[i].namePtr || rawBuf[i].nameLen <= 0)
			continue;

		PlayerInfo info;
		info.pingMs = static_cast<uint32_t>(rawBuf[i].ping);
		info.duration = 0.0f;

		// The SDK FString is wchar_t internally; convert to narrow string.
		const wchar_t* wstr = reinterpret_cast<const wchar_t*>(rawBuf[i].namePtr);
		int wlen = rawBuf[i].nameLen - 1; // exclude null terminator
		if (wlen > 0)
		{
			int mbLen = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
			if (mbLen > 0)
			{
				info.name.resize(static_cast<size_t>(mbLen));
				WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, info.name.data(), mbLen, nullptr, nullptr);
			}
		}

		if (!info.name.empty())
			players.push_back(std::move(info));
	}

	ServerState::Get().UpdatePlayers(std::move(players));
#else
	(void)worldPtr;
#endif
}

// ---------------------------------------------------------------------------
// Background refresh thread – wakes every 5 seconds to update player cache
// ---------------------------------------------------------------------------
static void RefreshLoop()
{
	while (g_refreshRunning)
	{
		void* world = g_currentWorld.load(std::memory_order_acquire);
		if (world)
			CollectPlayers(world);

		// Sleep in 500 ms chunks so shutdown is responsive
		for (int i = 0; i < 10 && g_refreshRunning; ++i)
			Sleep(500);
	}
}

// ---------------------------------------------------------------------------
// Rcon namespace – public API
// ---------------------------------------------------------------------------
void Rcon::Init()
{
	LOG_INFO("[Rcon] Initialising RCON / Query subsystem...");

	const uint16_t    port = ReadRconPort();
	const std::string password = ReadRconPassword();

	if (port == 0 || password.empty())
	{
		if (port == 0)
			LOG_INFO("[Rcon] No -RconPort= provided – RCON subsystem will not start");
		if (password.empty())
			LOG_INFO("[Rcon] No -RconPassword= provided – RCON subsystem will not start");
		LOG_INFO("[Rcon] To enable RCON, launch with: -RconPort=<port> -RconPassword=<password>");
		return;
	}

	// Initialise Winsock (version 2.2)
	WSADATA wsaData{};
	const int wsaErr = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaErr != 0)
	{
		LOG_ERROR("[Rcon] WSAStartup failed: %d", wsaErr);
		return;
	}

	const std::string servName = ReadSessionName();

	ServerState::Get().SetServerName(servName);

	LOG_INFO("[Rcon] Query port : %d", port);
	LOG_INFO("[Rcon] Server name: %s", servName.c_str());
	LOG_INFO("[Rcon] RCON password is set");

	// Register built-in commands
	auto& cmds = CommandHandler::Get();
	Cmd_Players::Register(cmds);
	Cmd_Stop::Register(cmds);
	Cmd_Save::Register(cmds);

	// Start UDP query server (always)
	g_queryServer.Start(port);

	// Start TCP RCON server
	g_rconServer.Start(port, password);

	// Launch background player-refresh thread
	g_refreshRunning = true;
	g_refreshThread = std::thread(RefreshLoop);

	LOG_INFO("[Rcon] Subsystem ready");
}

void Rcon::Shutdown()
{
	LOG_INFO("[Rcon] Shutting down...");

	// Clear world pointer first so the refresh thread stops accessing engine memory
	g_currentWorld.store(nullptr, std::memory_order_release);

	// Stop refresh thread
	g_refreshRunning = false;
	if (g_refreshThread.joinable())
		g_refreshThread.join();

	// Stop network servers
	g_rconServer.Stop();
	g_queryServer.Stop();

	WSACleanup();

	LOG_INFO("[Rcon] Shutdown complete");
}

void Rcon::OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	LOG_INFO("[Rcon] World begin play: %s", worldName ? worldName : "(null)");

	g_currentWorld.store(static_cast<void*>(world), std::memory_order_release);

	if (worldName && *worldName)
		ServerState::Get().SetWorldName(worldName);

	// Do an immediate player collection on the game thread (it's the safest moment)
	CollectPlayers(world);

	// Drain any tasks queued by RCON command handlers that need the game thread
	GameThreadDispatch::Drain();
}

void Rcon::OnExperienceLoadComplete()
{
	LOG_INFO("[Rcon] Experience load complete – refreshing player state");
	void* world = g_currentWorld.load(std::memory_order_acquire);
	CollectPlayers(world);

	// Drain any tasks queued by RCON command handlers that need the game thread
	GameThreadDispatch::Drain();
}

void Rcon::OnTick(float /*deltaSeconds*/)
{
	// Drain any tasks queued by RCON command handlers that need the game thread.
	// This runs every frame on the game thread, so posted tasks (e.g. save
	// commands) are picked up promptly rather than waiting for a one-shot event.
	GameThreadDispatch::Drain();
}
