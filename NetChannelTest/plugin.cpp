#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "plugin_network_helpers.h"

#include <mutex>
#include <cstdio>
#include <cstring>

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// POD packet sent from server to client every N ticks
struct TestPacket
{
	uint32_t tickCount;   // server tick counter at time of send
	uint32_t sendCount;   // number of packets sent so far this session
	float    uptime;      // seconds since plugin init (filled by server)
	uint8_t  pad[4];
};

// POD packet sent from client to server as an acknowledgement
struct AckPacket
{
	uint32_t ackedSendCount; // echoes TestPacket::sendCount that triggered this ack
	uint8_t  pad[4];
};

static uint32_t s_tickCount = 0;
static uint32_t s_sendCount = 0;
static float    s_uptime = 0.0f;

// ---------------------------------------------------------------------------
// World name — written from the world-begin-play callback, read from the
// HTTP route callback (different threads). Protected by a mutex.
// ---------------------------------------------------------------------------
static std::mutex  s_worldNameMutex;
static char        s_worldName[256] = "<unknown>";

static void OnAnyWorldBeginPlay(SDK::UWorld* /*world*/, const char* worldName)
{
	std::lock_guard lock(s_worldNameMutex);
	if (worldName && worldName[0])
		strncpy_s(s_worldName, worldName, _TRUNCATE);
	else
		strncpy_s(s_worldName, "<unknown>", _TRUNCATE);
}

// Typed alias so the function pointer matches PluginAnyWorldBeginPlayCallback exactly.
static const PluginAnyWorldBeginPlayCallback k_OnAnyWorldBeginPlay = &OnAnyWorldBeginPlay;

static void OnApiRoute(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
	// Copy stats atomically enough for a debug endpoint (plain reads of
	// 32-bit values are atomic on x86-64; uptime is a float so read once).
	const uint32_t ticks     = s_tickCount;
	const uint32_t sends     = s_sendCount;
	const float  uptime    = s_uptime;

	char worldName[256];
	{
		std::lock_guard lock(s_worldNameMutex);
		strncpy_s(worldName, s_worldName, _TRUNCATE);
	}

	static char s_jsonBuf[512];
	const int written = snprintf(s_jsonBuf, sizeof(s_jsonBuf),
		"{\n"
		"  \"plugin\": \"NetChannelTest\",\n"
		"  \"worldName\": \"%s\",\n"
		"  \"tickCount\": %u,\n"
		"  \"sendCount\": %u,\n"
		"  \"uptime\": %.2f\n"
		"}",
		worldName, ticks, sends, uptime);

	resp->statusCode  = 200;
	resp->contentType = "application/json";
	resp->body     = s_jsonBuf;
	resp->bodyLen     = (written > 0) ? static_cast<size_t>(written) : 0;

	if (g_self)
		LOG_DEBUG("HTTP /api/ served: world=%s ticks=%u sends=%u uptime=%.1f",
			worldName, ticks, sends, uptime);
}

// Server->Client receive handle (client side)
static PluginNetworkMessageCallback s_receiveHandle = nullptr;
// Client->Server receive handle (server side)
static PluginNetworkServerMessageCallback s_serverReceiveHandle = nullptr;

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"NetChannelTest",
	MODLOADER_BUILD_TAG,
	"ModLoader Dev",
	"Tests bidirectional network channel: server->client TestPacket + client->server AckPacket",
	PLUGIN_INTERFACE_VERSION
};

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		g_self = self;

		LOG_INFO("Plugin initializing...");

		NetChannelTestConfig::Config::Initialize(self);

		if (!NetChannelTestConfig::Config::IsEnabled())
		{
			LOG_WARN("Disabled in config");
			return true;
		}

		if (!self->hooks->Network)
		{
			LOG_WARN("IPluginNetworkChannel not available (loader too old?)");
			return true;
		}

		if (self->hooks->Network->IsServer())
		{
			const int interval = NetChannelTestConfig::Config::GetSendIntervalTicks();
			LOG_INFO("Server mode -- will send TestPacket every %d ticks, listening for AckPackets", interval);

			// Register handler for AckPackets arriving from clients
			s_serverReceiveHandle = Network::OnServerReceive<AckPacket>(
				self->hooks, self,
				[](void* /*senderPC*/, const AckPacket& ack)
				{
					LOG_INFO("Received AckPacket from client: ackedSendCount=%u", ack.ackedSendCount);
				});

			/*
			self->hooks->Engine->RegisterOnTick([](float deltaTime) {
				if (!g_self || !g_self->hooks->Network) return;

				s_uptime += deltaTime;
				++s_tickCount;

				const int interval = NetChannelTestConfig::Config::GetSendIntervalTicks();
				if (interval <= 0 || (s_tickCount % static_cast<uint32_t>(interval)) != 0)
					return;

				++s_sendCount;

				TestPacket pkt{};
				pkt.tickCount = s_tickCount;
				pkt.sendCount = s_sendCount;
				pkt.uptime    = s_uptime;

				Network::SendPacketToAllClients(g_self->hooks, g_self, pkt);

				LOG_DEBUG("Sent TestPacket #%u (tick=%u uptime=%.1fs)", s_sendCount, s_tickCount, s_uptime);
			});
			*/

			// ---------------------------------------------------------------
			// HTTP routes (v22, server only)
			// ---------------------------------------------------------------
			if (self->hooks->HttpServer)
			{
				// Track world name for the API route
				self->hooks->World->RegisterOnAnyWorldBeginPlay(k_OnAnyWorldBeginPlay);

				// Raw JSON API: GET /netchanneltest/api/
				if (self->hooks->HttpServer->AddRawRoute(self, "api", OnApiRoute))
					LOG_INFO("HTTP raw route registered: /netchanneltest/api/");
				else
					LOG_WARN("HTTP raw route registration failed (already registered?)");

				/* Static file route: /netchanneltest/ui/  ->  Plugins\NetChannelTest\ui\ */
				if (self->hooks->HttpServer->AddRoute(self, "ui"))
					LOG_INFO("HTTP static route registered: /netchanneltest/ui/");
				else
					LOG_WARN("HTTP static route registration failed (already registered?)");
			}
			else
			{
				LOG_INFO("HttpServer not available (client build or loader < v22) -- HTTP routes skipped");
			}
		}
		else
		{
			LOG_INFO("Client mode -- registering TestPacket receive handler, will ack each packet");

			// Register handler for TestPackets arriving from server;
			// send an AckPacket back to the server for each one received
			s_receiveHandle = Network::OnReceive<TestPacket>(
				self->hooks, self,
				[](const TestPacket& pkt)
				{
					LOG_INFO("Received TestPacket: sendCount=%u tickCount=%u uptime=%.1fs",
						pkt.sendCount, pkt.tickCount, pkt.uptime);

					if (!g_self || !g_self->hooks->Network) return;

					AckPacket ack{};
					ack.ackedSendCount = pkt.sendCount;

					Network::SendPacketToServer(g_self->hooks, g_self, ack);

					LOG_DEBUG("Sent AckPacket for sendCount=%u", ack.ackedSendCount);
				});
		}

		LOG_INFO("Plugin initialized");
		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_self && g_self->hooks->Network)
		{
			if (g_self->hooks->Network->IsServer())
			{
				if (s_serverReceiveHandle)
				{
					g_self->hooks->Network->UnregisterServerMessageHandler(
						g_self, typeid(AckPacket).name(), s_serverReceiveHandle);
					s_serverReceiveHandle = nullptr;
				}

				// Unregister HTTP routes and world name callback
				if (g_self->hooks->HttpServer)
				{
					g_self->hooks->HttpServer->RemoveRawRoute(g_self, "api");
					g_self->hooks->HttpServer->RemoveRoute(g_self, "ui");
					LOG_INFO("HTTP routes unregistered");
				}

				if (g_self->hooks->World)
					g_self->hooks->World->UnregisterOnAnyWorldBeginPlay(k_OnAnyWorldBeginPlay);
			}
			else
			{
				if (s_receiveHandle)
				{
					g_self->hooks->Network->UnregisterMessageHandler(
						g_self, typeid(TestPacket).name(), s_receiveHandle);
					s_receiveHandle = nullptr;
				}
			}
		}

		s_tickCount = 0;
		s_sendCount = 0;
		s_uptime    = 0.0f;

		{
			std::lock_guard lock(s_worldNameMutex);
			strncpy_s(s_worldName, "<unknown>", _TRUNCATE);
		}

		g_self = nullptr;
	}

} // extern "C"
