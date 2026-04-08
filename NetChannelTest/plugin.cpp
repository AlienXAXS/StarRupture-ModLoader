#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "plugin_network_helpers.h"

static IPluginLogger* g_logger = nullptr;
static IPluginConfig* g_config = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks* g_hooks = nullptr;

IPluginLogger* GetLogger() { return g_logger; }
IPluginConfig* GetConfig() { return g_config; }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks* GetHooks() { return g_hooks; }

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

	__declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks)
	{
		g_logger = logger;
		g_config = config;
		g_scanner = scanner;
		g_hooks = hooks;

		LOG_INFO("Plugin initializing...");

		NetChannelTestConfig::Config::Initialize(config);

		if (!NetChannelTestConfig::Config::IsEnabled())
		{
			LOG_WARN("Disabled in config");
			return true;
		}

		if (!hooks->Network)
		{
			LOG_WARN("IPluginNetworkChannel not available (loader too old?)");
			return true;
		}

		if (hooks->Network->IsServer())
		{
			const int interval = NetChannelTestConfig::Config::GetSendIntervalTicks();
			LOG_INFO("Server mode -- will send TestPacket every %d ticks, listening for AckPackets", interval);

			// Register handler for AckPackets arriving from clients
			s_serverReceiveHandle = Network::OnServerReceive<AckPacket>(
				hooks, "NetChannelTest",
				[](void* /*senderPC*/, const AckPacket& ack)
				{
					LOG_INFO("Received AckPacket from client: ackedSendCount=%u", ack.ackedSendCount);
				});

			hooks->Engine->RegisterOnTick([](float deltaTime) {
				if (!g_hooks || !g_hooks->Network) return;

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

				Network::SendPacketToAllClients(g_hooks, "NetChannelTest", pkt);

				LOG_DEBUG("Sent TestPacket #%u (tick=%u uptime=%.1fs)", s_sendCount, s_tickCount, s_uptime);
			});
		}
		else
		{
			LOG_INFO("Client mode -- registering TestPacket receive handler, will ack each packet");

			// Register handler for TestPackets arriving from server;
			// send an AckPacket back to the server for each one received
			s_receiveHandle = Network::OnReceive<TestPacket>(
				hooks, "NetChannelTest",
				[](const TestPacket& pkt)
				{
					LOG_INFO("Received TestPacket: sendCount=%u tickCount=%u uptime=%.1fs",
						pkt.sendCount, pkt.tickCount, pkt.uptime);

					if (!g_hooks || !g_hooks->Network) return;

					AckPacket ack{};
					ack.ackedSendCount = pkt.sendCount;

					Network::SendPacketToServer(g_hooks, "NetChannelTest", ack);

					LOG_DEBUG("Sent AckPacket for sendCount=%u", ack.ackedSendCount);
				});
		}

		LOG_INFO("Plugin initialized");
		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_hooks && g_hooks->Network)
		{
			if (g_hooks->Network->IsServer())
			{
				if (s_serverReceiveHandle)
				{
					g_hooks->Network->UnregisterServerMessageHandler(
						"NetChannelTest", typeid(AckPacket).name(), s_serverReceiveHandle);
					s_serverReceiveHandle = nullptr;
				}
			}
			else
			{
				if (s_receiveHandle)
				{
					g_hooks->Network->UnregisterMessageHandler(
						"NetChannelTest", typeid(TestPacket).name(), s_receiveHandle);
					s_receiveHandle = nullptr;
				}
			}
		}

		s_tickCount = 0;
		s_sendCount = 0;
		s_uptime    = 0.0f;

		g_logger  = nullptr;
		g_config  = nullptr;
		g_scanner = nullptr;
		g_hooks   = nullptr;
	}

} // extern "C"
