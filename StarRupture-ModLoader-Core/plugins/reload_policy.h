#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include "plugins/plugin_interface.h"

// When a plugin may be loaded and unloaded while the game runs: the two
// PluginReload settings in PluginInfo::mapFlags, the plugin's busy reason, and
// whether a game world is running. The plugin manager asks here before it
// touches a plugin, so a refusal leaves the plugin exactly as it was.
//
// Header only and free of engine and game state, so the tests build it as is.
namespace ReloadPolicy
{
	struct Settings
	{
		PluginReload load   = PluginReload::ANYTIME;
		PluginReload unload = PluginReload::ANYTIME;
	};

	inline constexpr const char* kMenuReason    = "Return to the main menu to reload this plugin";
	inline constexpr const char* kRestartReason = "This plugin requires a game restart";
	inline constexpr const char* kNoUnloadReason = "Unload not supported: this plugin requires a game restart";

	inline constexpr size_t kBusyReasonMax = 80;

	// The settings a plugin declared. Only a plugin that declares interface 73
	// or later has them: the bits are 0 in everything built before, but an older
	// plugin's mapFlags is not ours to interpret. A value no setting uses
	// (a plugin from a future interface) is read as RESTART, the safe side.
	inline PluginReload ReadOne(uint32_t mapFlags, uint32_t shift)
	{
		switch ((mapFlags >> shift) & PLUGIN_RELOAD_MASK)
		{
		case 0:  return PluginReload::ANYTIME;
		case 1:  return PluginReload::MENU;
		default: return PluginReload::RESTART;
		}
	}

	inline Settings Read(int interfaceVersion, uint32_t mapFlags)
	{
		Settings s;
		if (interfaceVersion < 73)
			return s;
		s.load   = ReadOne(mapFlags, PLUGIN_RELOAD_LOAD_SHIFT);
		s.unload = ReadOne(mapFlags, PLUGIN_RELOAD_UNLOAD_SHIFT);
		return s;
	}

	// Whether a gameplay world (ChimeraMain) is running now. Fed by the loader's
	// own world begin and end hooks; false at startup, on the splash and at the
	// main menu. If the end hook never installed it stays true after the first
	// world, which refuses MENU plugins: the safe way to be wrong.
	inline std::atomic<bool>& WorldFlag()
	{
		static std::atomic<bool> running{ false };
		return running;
	}
	inline bool GameplayWorldRunning()            { return WorldFlag().load(std::memory_order_acquire); }
	inline void SetGameplayWorldRunning(bool on)  { WorldFlag().store(on, std::memory_order_release); }

	// World names as the world events deliver them.
	inline bool IsGameplayWorldName(const char* name) { return name && std::string(name).find("ChimeraMain") != std::string::npos; }
	inline bool IsMainMenuWorldName(const char* name) { return name && std::string(name).find("Map_MainMenu") != std::string::npos; }

	// A world began play: a gameplay world starts the session; the main menu
	// world means there is none (the end event of the last one may not have come).
	inline void NoteWorldBegan(const char* name)
	{
		if (IsGameplayWorldName(name))
			SetGameplayWorldRunning(true);
		else if (IsMainMenuWorldName(name))
			SetGameplayWorldRunning(false);
	}

	// A world ended play.
	inline void NoteWorldEnded(const char* name)
	{
		if (IsGameplayWorldName(name))
			SetGameplayWorldRunning(false);
	}

	// Why a plugin with this load setting may not be loaded now, or "" if it
	// may. startup: the loader is doing its startup plugin load.
	inline std::string LoadRefusal(PluginReload load, bool startup, bool worldRunning)
	{
		if (load == PluginReload::RESTART && !startup)
			return kRestartReason;
		if (load == PluginReload::MENU && worldRunning)
			return kMenuReason;
		return {};
	}

	// Why a loaded plugin may not be unloaded now, or "" if it may. busy is the
	// plugin's own reason, "" when it has none. Never asked at game shutdown.
	inline std::string UnloadRefusal(PluginReload unload, const std::string& busy, bool worldRunning)
	{
		if (unload == PluginReload::RESTART)
			return kNoUnloadReason;
		if (unload == PluginReload::MENU && worldRunning)
			return kMenuReason;
		if (!busy.empty())
			return "Unload blocked: " + busy;
		return {};
	}

	// A reload is the unload and then the load: refused if either is, and
	// decided before the first of them starts.
	inline std::string ReloadRefusal(const Settings& s, const std::string& busy, bool loaded, bool worldRunning)
	{
		if (loaded)
			if (std::string why = UnloadRefusal(s.unload, busy, worldRunning); !why.empty())
				return why;
		return LoadRefusal(s.load, false, worldRunning);
	}

	// The busy reasons. Keyed by the plugin's IPluginSelf, which is stable for
	// the life of the process (plugin records are never freed one by one). Kept
	// apart from the plugin manager's lock on purpose: a plugin sets it from any
	// thread, including from inside PluginShutdown while that lock is held.
	namespace detail
	{
		inline std::mutex& BusyLock() { static std::mutex m; return m; }
		inline std::unordered_map<const IPluginSelf*, std::string>& BusyMap()
		{
			static std::unordered_map<const IPluginSelf*, std::string> m;
			return m;
		}
	}

	// IPluginEngineEvents::SetBusyReason. Null or empty clears it. Control
	// characters become spaces and the text is cut at kBusyReasonMax.
	inline void SetBusyReason(const IPluginSelf* self, const char* reason)
	{
		if (!self)
			return;
		std::string text;
		for (const char* p = reason; p && *p && text.size() < kBusyReasonMax; ++p)
			text += (static_cast<unsigned char>(*p) < 0x20) ? ' ' : *p;

		std::lock_guard<std::mutex> lock(detail::BusyLock());
		if (text.empty())
			detail::BusyMap().erase(self);
		else
			detail::BusyMap()[self] = std::move(text);
	}

	inline std::string BusyReason(const IPluginSelf* self)
	{
		std::lock_guard<std::mutex> lock(detail::BusyLock());
		const auto it = detail::BusyMap().find(self);
		return it == detail::BusyMap().end() ? std::string() : it->second;
	}
}
