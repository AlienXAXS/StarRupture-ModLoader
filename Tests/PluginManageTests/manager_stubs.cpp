// What plugin_manager.cpp and auto_updater.cpp reach for outside the files under
// test. This holds the move hook, the two folder paths (beside the running exe,
// as beside the game's) and the game thread. The logging, UI, version map and
// registry sinks go below as the linker names them; all of them do nothing.
#include "core/startup_utils.h"
#include "utils/game_thread_dispatch.h"
#include "logging/log.h"
#include "logging/logger.h"
#include "logging/logger_interface.h"
#include "config/config_manager.h"
#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/version_map.h"
#include "memory_scanner/version_feed.h"
#include "network_channel/network_channel.h"
#include "hooks/hooks_interface.h"
#include "hooks/hook_broker.h"
#include "hooks/game/game_menu/game_menu_registry.h"
#include "hooks/input/keybind_registry.h"
#include "hooks/input/mouse_wheel_registry.h"
#include "core/ui_state.h"
#include "console/plugin_console.h"
#include "plugins/pak_registry.h"
#include "plugins/plugin_catalog.h"
#include "plugins/plugin_hook_report.h"
#include "preload/preload_manager.h"
#include "auto_update/feed_fetch.h"
#include "auto_update/http_get.h"
#include "auto_update/mod_index_fetcher.h"
#include "auto_update/mod_index_signed.h"
#include "UI/splash_window.h"
#include "UI/modloader_window.h"
#include "UI/plugin_panel_registry.h"
#include "UI/plugin_widget_registry.h"
#include "UI/image_library.h"
#include "UI/global_settings.h"
#include "UI/toast.h"
#include "UI/update_notice_window.h"
#include "manager_tests.h"

#undef MoveFileExW

namespace
{
	int g_moves = 0;
	int g_failFirst = 0;
	int g_failSecond = 0;
	const void* g_cutQuery = nullptr;
}

const void* RegisteredCutQuery()
{
	return g_cutQuery;
}

void FailMoves(int first, int second)
{
	g_moves = 0;
	g_failFirst = first;
	g_failSecond = second;
}

BOOL TestMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
	++g_moves;
	if (g_moves == g_failFirst || g_moves == g_failSecond)
	{
		SetLastError(ERROR_SHARING_VIOLATION);
		return FALSE;
	}
	return MoveFileExW(from, to, flags);
}

std::wstring GetExeDir()
{
	wchar_t exe[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring path = exe;
	return path.substr(0, path.find_last_of(L'\\') + 1);
}

std::wstring GetModLoaderDirPath(const wchar_t* filename)
{
	return GetExeDir() + L"ModLoader\\" + filename;
}

// The game thread is the thread that asks.
namespace GameThreadDispatch
{
	void PostVoid(std::function<void()> work, HMODULE) { work(); }
	void ForgetModule(HMODULE) {}
}

namespace ModLoaderLogger
{
	void LogInfo(const wchar_t*, ...) {}
	void LogWarn(const wchar_t*, ...) {}
	void LogError(const wchar_t*, ...) {}
	void LogMessage(const wchar_t*, ...) {}
	IPluginConfig* GetPluginConfig() { return nullptr; }
	IPluginLogger* GetPluginLogger() { return nullptr; }
	IPluginHooks* GetPluginHooks() { return nullptr; }
	IPluginHookScanner* GetPluginHookScanner() { return nullptr; }
	unsigned SdkGlobalsCallsOnThisThread() { return 0; }
	void ForgetPluginCallbacks(HMODULE) {}
	void ForgetPluginSchema(const char*) {}
}

namespace Splash
{
	void SetSubStatus(const wchar_t*) {}
	void SetSubProgress(float) {}
	void ClearSubBar() {}
}

namespace UI
{
	namespace GlobalSettings
	{
		bool GetRestartCrashedPlugins() { return false; }
		void SetUpdateAvailable(bool) {}
	}
	namespace ModLoaderWindow { void LoadBlockingStateForPlugin(const char*) {} }
	namespace PluginPanelRegistry
	{
		bool ForgetModule(HMODULE, DWORD) { return true; }
		void ForgetConfigCallbacks(const IPluginSelf*) {}
		void SetCurrentRegistrationPlugin(const char*) {}
	}
	namespace PluginWidgetRegistry { bool ForgetModule(HMODULE, DWORD) { return true; } }
	namespace Toast { void Show(const std::string&, const std::string&) {} }
	namespace UpdateNoticeWindow
	{
		void AddUpdatedPlugin(const char*, const char*, const char*, const char*) {}
		void SetOnShown(std::function<void()>) {}
		void ShowVortexNotice() {}
	}
}

namespace VersionMap
{
	Exe RunningExe() { return {}; }
	PluginGameBuild ReadPluginGameBuild(const PluginInfo& info)
	{
		PluginGameBuild build{};
		build.mapFlags = info.mapFlags; // carries the load and unload settings
		return build;
	}
	std::string DescribePluginGameBuild(const PluginGameBuild&) { return {}; }
	std::string OffBuildRefusal(const Exe&, int, const PluginGameBuild&, bool) { return {}; }
	std::string LayoutRefusal(const Exe&, int, const PluginGameBuild&) { return {}; }
}

namespace PluginHookReport
{
	void BeginSession(const IPluginSelf*, const char*, const char*, bool, bool) {}
	void EndSession(const IPluginSelf*, bool* refusedOut) { if (refusedOut) *refusedOut = false; }
	void RecordSessionCrash(const IPluginSelf*, const char*) {}
	void ForgetFeatures(const IPluginSelf*) {}
	std::vector<FeatureInfo> Features() { return {}; }
}

namespace PreloadManager
{
	std::vector<Record> GetRecords() { return {}; }
	std::vector<std::string> FindNewDlls() { return {}; }
}

namespace GameMenu::Registry { void ForgetPlugin(const char*) {} }
namespace NetworkChannel { void PluginFeaturesDeclared() {} }
namespace Hooks::Broker { void SetCutQuery(CutQuery query) { g_cutQuery = reinterpret_cast<const void*>(query); } }
namespace Hooks::Input { void ForgetPluginBlocking(const char*) {} }
namespace Hooks::MouseWheel { void ForgetModule(HMODULE) {} }
namespace Hooks::Broker { size_t RetireModule(HMODULE, size_t) { return 0; } }
namespace ImageLibrary { void ForgetPlugin(const IPluginSelf*) {} }
namespace PakRegistry { void ForgetPlugin(const char*) {} }
namespace PluginConsole { void ForgetPlugin(const char*) {} }
namespace PluginCatalog { int CopyAccessorText(char*, size_t) { return 0; } }
namespace UIState { void ForgetModule(HMODULE) {} }

// Nothing here reaches the network.
std::string FormatWinHttpError(unsigned long) { return {}; }
std::string HttpGet(const char*, const char*, const HttpGetOptions&) { return {}; }
std::string HttpGetUpdate(const std::string&, const std::string&, UpdateHosts::Owner, const char*) { return {}; }
namespace VersionFeed
{
	const std::vector<PublicKey>& CompiledKeys()
	{
		static const std::vector<PublicKey> none;
		return none;
	}
}
namespace FeedFetch
{
	Result RefreshCatalog(const Config&) { return {}; }
	void Stage3(const Config&) {}
}
namespace ModIndex
{
	FetchFn MakeFetch(Source) { return {}; }
	Fetcher::Fetcher(std::string, FetchFn, std::shared_ptr<const bool>) {}
	Fetcher::~Fetcher() {}
}
