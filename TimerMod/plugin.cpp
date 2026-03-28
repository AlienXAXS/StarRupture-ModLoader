#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "timer_tracker.h"
#include "data_export.h"
#include "hud_overlay.h"

#include "Engine_classes.hpp"

// ---------------------------------------------------------------------------
// Global plugin interface pointers
// ---------------------------------------------------------------------------
static IPluginLogger*  g_logger  = nullptr;
static IPluginConfig*  g_config  = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks*   g_hooks   = nullptr;

IPluginLogger*  GetLogger()  { return g_logger; }
IPluginConfig*  GetConfig()  { return g_config; }
IPluginScanner* GetScanner() { return g_scanner; }
IPluginHooks*   GetHooks()   { return g_hooks; }

// ---------------------------------------------------------------------------
// Plugin metadata
// ---------------------------------------------------------------------------
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"RuptureTimer",
	MODLOADER_BUILD_TAG,
	"Nhimself",
	"Tracks the rupture wave timer. Exports phase/countdown data to JSON for StreamDeck integration and optionally renders an in-game HUD overlay.",
	PLUGIN_INTERFACE_VERSION
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_worldReady = false;
static RuptureTimer::TimerState s_lastState{};

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
{
	if (!world || !worldName) return;

	// Only track the main gameplay world
	if (std::string_view(worldName).find("ChimeraMain") == std::string_view::npos)
	{
		if (s_worldReady)
		{
			LOG_INFO("World changed to non-gameplay world (%s) — pausing rupture timer tracking", worldName);
			s_worldReady = false;
		}
		return;
	}

	LOG_INFO("World ready: %s — starting rupture timer tracking", worldName);
	s_worldReady = true;
	DataExport::EnsureOutputDir();
}

static void OnSaveLoaded()
{
	LOG_INFO("Save loaded — reading initial rupture timer state");
	s_lastState = RuptureTimer::ReadCurrentState();
	if (s_lastState.valid)
	{
		LOG_INFO("  Phase: %s | Remaining: %.1fs | Wave #%d | Type: %s",
			s_lastState.phaseName,
			s_lastState.phaseRemainingSeconds,
			s_lastState.waveNumber,
			s_lastState.waveTypeName);
		HudOverlay::SetState(s_lastState);
	}
	else
	{
		LOG_WARN("  Timer state not available yet (game state not fully loaded?)");
	}
}

static void OnEngineTick(float deltaSeconds)
{
	if (!s_worldReady) return;

	// Heartbeat log every 5s so we can confirm tick is firing and delta is valid
	static float s_logAccum = 0.0f;
	s_logAccum += deltaSeconds;
	if (s_logAccum >= 5.0f)
	{
		s_logAccum = 0.0f;
		LOG_DEBUG("Tick alive — deltaSeconds=%.4f", deltaSeconds);
	}

	s_lastState = RuptureTimer::ReadCurrentState();
	DataExport::Update(deltaSeconds, s_lastState);
	HudOverlay::SetState(s_lastState);
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------
extern "C" {

__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks)
{
	g_logger  = logger;
	g_config  = config;
	g_scanner = scanner;
	g_hooks   = hooks;

	LOG_INFO("RuptureTimer initializing...");

	RuptureTimerConfig::Config::Initialize(config);

	if (!RuptureTimerConfig::Config::IsEnabled())
	{
		LOG_WARN("RuptureTimer is disabled in config");
		return true;
	}

	if (!hooks)
	{
		LOG_ERROR("hooks interface is null — cannot register callbacks");
		return false;
	}

	if (hooks->World)
	{
		hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		hooks->World->RegisterOnSaveLoaded(OnSaveLoaded);
		LOG_DEBUG("Registered world/save callbacks");
	}

	if (hooks->Engine)
	{
		hooks->Engine->RegisterOnTick(OnEngineTick);
		LOG_DEBUG("Registered engine tick callback");
	}

	// HUD overlay — only install the hook if the overlay is enabled in config.
	// The hook itself is cheap (just a PostRender trampoline), but we skip the
	// pattern scan entirely when the feature is off to keep startup clean.
	if (RuptureTimerConfig::Config::ShouldShowOverlay())
	{
		if (!HudOverlay::Install(scanner, hooks))
			LOG_WARN("HUD overlay could not be installed — in-game display will be unavailable");
	}
	else
	{
		LOG_DEBUG("HUD overlay disabled in config (HUD.ShowOverlay=false)");
	}

	LOG_INFO("RuptureTimer initialized — JSON output: %s | HUD overlay: %s",
		RuptureTimerConfig::Config::GetJsonFilePath(),
		RuptureTimerConfig::Config::ShouldShowOverlay() ? "enabled" : "disabled");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("RuptureTimer shutting down...");

	s_worldReady = false;

	HudOverlay::Remove(g_hooks);

	if (auto* hooks = GetHooks())
	{
		if (hooks->World)
		{
			hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
			hooks->World->UnregisterOnSaveLoaded(OnSaveLoaded);
		}
		if (hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnEngineTick);
	}

	g_logger  = nullptr;
	g_config  = nullptr;
	g_scanner = nullptr;
	g_hooks   = nullptr;
}

} // extern "C"
