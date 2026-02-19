#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

// Global plugin interface pointers
static IPluginLogger* g_logger   = nullptr;
static IPluginConfig* g_config   = nullptr;
static IPluginScanner* g_scanner = nullptr;
static IPluginHooks* g_hooks     = nullptr;

// Hook handle for cleanup
static HookHandle g_hookHandle = nullptr;

// Plugin metadata
static PluginInfo s_pluginInfo = {
	"AntiLogSpam",
	"1.0.0",
	"AlienX",
	"Silently suppresses log spam where possible",
	PLUGIN_INTERFACE_VERSION
};

// Helper functions to access plugin interfaces
IPluginLogger* GetLogger()  { return g_logger;  }
IPluginConfig* GetConfig()  { return g_config;  }
IPluginScanner* GetScanner(){ return g_scanner; }
IPluginHooks* GetHooks()    { return g_hooks;   }

// -----------------------------------------------------------------------
// UObjectBase::IsValidLowLevelFast hook
// Signature: char __fastcall UObjectBase::IsValidLowLevelFast(UObjectBase* this, bool bRecursive)
// -----------------------------------------------------------------------

typedef char(__fastcall* IsValidLowLevelFast_t)(void* thisPtr, bool bRecursive);
static IsValidLowLevelFast_t g_originalIsValidLowLevelFast = nullptr;

static char __fastcall Hook_IsValidLowLevelFast(void* thisPtr, bool bRecursive)
{
	if (thisPtr == nullptr)
		return 0;

	return g_originalIsValidLowLevelFast(thisPtr, bRecursive);
}

// -----------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------

// Engine init callback - called when UE5 engine finishes initialization
static void OnEngineInit()
{
	LOG_INFO("Engine initialized - applying patch...");

	if (!AntiLogSpamConfig::Config::IsEnabled())
	{
		LOG_WARN("PatchMiningGunLogSpam is DISABLED in config - patch will not be applied");
		return;
	}

	// Scan for UObjectBase::IsValidLowLevelFast
	uintptr_t addr = g_scanner->FindPatternInMainModule(
		"0F 82 ?? ?? ?? ?? F6 C3 ?? 74 ?? 80 3D");

	if (addr == 0)
	{
		LOG_ERROR("Pattern scan failed - could not locate UObjectBase::IsValidLowLevelFast");
		return; // Non-fatal: game still runs without the patch
	}

	LOG_INFO("Found UObjectBase::IsValidLowLevelFast at 0x%llX", (unsigned long long)addr);

	g_hookHandle = g_hooks->InstallHook(
		addr,
		(void*)&Hook_IsValidLowLevelFast,
		(void**)&g_originalIsValidLowLevelFast);

	if (g_hookHandle == nullptr)
	{
		LOG_ERROR("Failed to install hook on UObjectBase::IsValidLowLevelFast");
		return; // Non-fatal
	}

	LOG_INFO("Hook installed successfully - null-pointer calls will be silently suppressed");
}

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

		LOG_INFO("Plugin initializing...");

		AntiLogSpamConfig::Config::Initialize(config);
		LOG_INFO("Config initialized (PatchMiningGunLogSpam: %s)", AntiLogSpamConfig::Config::IsEnabled() ? "true" : "false");

		if (!hooks->RegisterEngineInitCallback)
		{
			LOG_ERROR("RegisterEngineInitCallback not available - loader version mismatch?");
			return false;
		}

		hooks->RegisterEngineInitCallback(OnEngineInit);
		LOG_INFO("Registered for engine init callback - patch will be applied when engine is ready");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_hooks && g_hookHandle)
		{
			g_hooks->RemoveHook(g_hookHandle);
			g_hookHandle = nullptr;
		}

		g_logger  = nullptr;
		g_config  = nullptr;
		g_scanner = nullptr;
		g_hooks   = nullptr;
	}

} // extern "C"
