#include "pch.h"
#include "keybind_registry.h"
#include "logging/logger.h"

#include <windows.h>
#include <mutex>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>

// Client-only feature — entire translation unit is a no-op on server/generic builds.
#ifdef MODLOADER_CLIENT_BUILD

namespace Hooks::Input
{
	// -----------------------------------------------------------------------
	// Key descriptor table — one entry per EModKey (in enum order)
	// -----------------------------------------------------------------------
	struct KeyDesc
	{
		EModKey key;
		int vk; // Win32 VK code (0 = no mapping)
		const char* name; // UE key name string
	};

	// Index must match the EModKey enum value order exactly.
	static const KeyDesc s_keyTable[] =
	{
		// Function keys
		{EModKey::F1, VK_F1, "F1"},
		{EModKey::F2, VK_F2, "F2"},
		{EModKey::F3, VK_F3, "F3"},
		{EModKey::F4, VK_F4, "F4"},
		{EModKey::F5, VK_F5, "F5"},
		{EModKey::F6, VK_F6, "F6"},
		{EModKey::F7, VK_F7, "F7"},
		{EModKey::F8, VK_F8, "F8"},
		{EModKey::F9, VK_F9, "F9"},
		{EModKey::F10, VK_F10, "F10"},
		{EModKey::F11, VK_F11, "F11"},
		{EModKey::F12, VK_F12, "F12"},

		// Letters
		{EModKey::A, 'A', "A"},
		{EModKey::B, 'B', "B"},
		{EModKey::C, 'C', "C"},
		{EModKey::D, 'D', "D"},
		{EModKey::E, 'E', "E"},
		{EModKey::F, 'F', "F"},
		{EModKey::G, 'G', "G"},
		{EModKey::H, 'H', "H"},
		{EModKey::I, 'I', "I"},
		{EModKey::J, 'J', "J"},
		{EModKey::K, 'K', "K"},
		{EModKey::L, 'L', "L"},
		{EModKey::M, 'M', "M"},
		{EModKey::N, 'N', "N"},
		{EModKey::O, 'O', "O"},
		{EModKey::P, 'P', "P"},
		{EModKey::Q, 'Q', "Q"},
		{EModKey::R, 'R', "R"},
		{EModKey::S, 'S', "S"},
		{EModKey::T, 'T', "T"},
		{EModKey::U, 'U', "U"},
		{EModKey::V, 'V', "V"},
		{EModKey::W, 'W', "W"},
		{EModKey::X, 'X', "X"},
		{EModKey::Y, 'Y', "Y"},
		{EModKey::Z, 'Z', "Z"},

		// Digit row
		{EModKey::Zero, '0', "Zero"},
		{EModKey::One, '1', "One"},
		{EModKey::Two, '2', "Two"},
		{EModKey::Three, '3', "Three"},
		{EModKey::Four, '4', "Four"},
		{EModKey::Five, '5', "Five"},
		{EModKey::Six, '6', "Six"},
		{EModKey::Seven, '7', "Seven"},
		{EModKey::Eight, '8', "Eight"},
		{EModKey::Nine, '9', "Nine"},

		// Control keys
		{EModKey::Escape, VK_ESCAPE, "Escape"},
		{EModKey::Tab, VK_TAB, "Tab"},
		{EModKey::CapsLock, VK_CAPITAL, "CapsLock"},
		{EModKey::SpaceBar, VK_SPACE, "SpaceBar"},
		{EModKey::Enter, VK_RETURN, "Enter"},
		{EModKey::BackSpace, VK_BACK, "BackSpace"},
		{EModKey::Delete, VK_DELETE, "Delete"},
		{EModKey::Insert, VK_INSERT, "Insert"},

		// Modifier keys
		{EModKey::LeftShift, VK_LSHIFT, "LeftShift"},
		{EModKey::RightShift, VK_RSHIFT, "RightShift"},
		{EModKey::LeftControl, VK_LCONTROL, "LeftControl"},
		{EModKey::RightControl, VK_RCONTROL, "RightControl"},
		{EModKey::LeftAlt, VK_LMENU, "LeftAlt"},
		{EModKey::RightAlt, VK_RMENU, "RightAlt"},

		// Navigation keys
		{EModKey::Up, VK_UP, "Up"},
		{EModKey::Down, VK_DOWN, "Down"},
		{EModKey::Left, VK_LEFT, "Left"},
		{EModKey::Right, VK_RIGHT, "Right"},
		{EModKey::Home, VK_HOME, "Home"},
		{EModKey::End, VK_END, "End"},
		{EModKey::PageUp, VK_PRIOR, "PageUp"},
		{EModKey::PageDown, VK_NEXT, "PageDown"},

		// Punctuation / OEM keys
		{EModKey::Tilde, VK_OEM_3, "Tilde"},
		{EModKey::Hyphen, VK_OEM_MINUS, "Hyphen"},
		{EModKey::Equals, VK_OEM_PLUS, "Equals"},
		{EModKey::LeftBracket, VK_OEM_4, "LeftBracket"},
		{EModKey::RightBracket, VK_OEM_6, "RightBracket"},
		{EModKey::Backslash, VK_OEM_5, "Backslash"},
		{EModKey::Semicolon, VK_OEM_1, "Semicolon"},
		{EModKey::Apostrophe, VK_OEM_7, "Apostrophe"},
		{EModKey::Comma, VK_OEM_COMMA, "Comma"},
		{EModKey::Period, VK_OEM_PERIOD, "Period"},
		{EModKey::Slash, VK_OEM_2, "Slash"},

		// Numpad digits
		{EModKey::NumPadZero, VK_NUMPAD0, "NumPadZero"},
		{EModKey::NumPadOne, VK_NUMPAD1, "NumPadOne"},
		{EModKey::NumPadTwo, VK_NUMPAD2, "NumPadTwo"},
		{EModKey::NumPadThree, VK_NUMPAD3, "NumPadThree"},
		{EModKey::NumPadFour, VK_NUMPAD4, "NumPadFour"},
		{EModKey::NumPadFive, VK_NUMPAD5, "NumPadFive"},
		{EModKey::NumPadSix, VK_NUMPAD6, "NumPadSix"},
		{EModKey::NumPadSeven, VK_NUMPAD7, "NumPadSeven"},
		{EModKey::NumPadEight, VK_NUMPAD8, "NumPadEight"},
		{EModKey::NumPadNine, VK_NUMPAD9, "NumPadNine"},

		// Numpad operators
		{EModKey::Add, VK_ADD, "Add"},
		{EModKey::Subtract, VK_SUBTRACT, "Subtract"},
		{EModKey::Multiply, VK_MULTIPLY, "Multiply"},
		{EModKey::Divide, VK_DIVIDE, "Divide"},
		{EModKey::Decimal, VK_DECIMAL, "Decimal"},

		// Mouse buttons
		{EModKey::LeftMouseButton, VK_LBUTTON, "LeftMouseButton"},
		{EModKey::RightMouseButton, VK_RBUTTON, "RightMouseButton"},
		{EModKey::MiddleMouseButton, VK_MBUTTON, "MiddleMouseButton"},
		{EModKey::ThumbMouseButton, VK_XBUTTON1, "ThumbMouseButton"},
		{EModKey::ThumbMouseButton2, VK_XBUTTON2, "ThumbMouseButton2"},
	};

	static constexpr int TABLE_SIZE = static_cast<int>(sizeof(s_keyTable) / sizeof(s_keyTable[0]));

	// -----------------------------------------------------------------------
	// Callback storage
	// -----------------------------------------------------------------------
	struct CallbackEntry
	{
		EModKey key;
		EModKeyEvent event;
		PluginKeybindCallback callback;
	};

	// Named-combo entries: registered via RegisterKeybindByName("Ctrl+F5", ...).
	// Stores the original combo string so UpdateKeybindByName can find and
	// update registrations in-place when the user rebinds in the config UI.
	struct NamedComboEntry
	{
		EModKey          key;
		EModKeyModifiers mods;
		EModKeyEvent     event;
		PluginKeybindCallback callback;
		char comboStr[64];  // original combo string, e.g. "Ctrl+F5" or "F5"
	};

	// Advanced-combo entries: registered via RegisterKeybindCombo (enum + explicit mods).
	// Uses PluginKeybindComboCallback — mods are passed back to the caller.
	struct ComboCallbackEntry
	{
		EModKey          key;
		EModKeyModifiers mods;
		EModKeyEvent     event;
		PluginKeybindComboCallback callback;
	};

	static std::vector<CallbackEntry>      s_callbacks;
	static std::vector<NamedComboEntry>    s_namedCombos;
	static std::vector<ComboCallbackEntry> s_comboCallbacks;
	// Blocking map: canonical combo string (e.g. "Ctrl+C") -> blocking enabled.
	// Protected by s_mutex. Default absent = non-blocking.
	static std::unordered_map<std::string, bool> s_blockingMap;
	static std::mutex s_mutex;
	static bool s_initialized = false;

	// -----------------------------------------------------------------------
	// Initialize / Shutdown
	// -----------------------------------------------------------------------
	void Initialize()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_initialized) return;
		s_initialized = true;
		ModLoaderLogger::LogInfo(L"[KeybindRegistry] Initialized (%d keys mapped)", TABLE_SIZE);
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_callbacks.clear();
		s_namedCombos.clear();
		s_comboCallbacks.clear();
		s_blockingMap.clear();
		s_initialized = false;
		ModLoaderLogger::LogInfo(L"[KeybindRegistry] Shutdown - all keybinds cleared");
	}

	// -----------------------------------------------------------------------
	// Lookup helpers
	// -----------------------------------------------------------------------
	int ModKeyToVK(EModKey key)
	{
		uint32_t idx = static_cast<uint32_t>(key);
		if (static_cast<int>(idx) >= TABLE_SIZE)
			return 0;
		return s_keyTable[idx].vk;
	}

	EModKey VKToModKey(int vk)
	{
		for (int i = 0; i < TABLE_SIZE; ++i)
		{
			if (s_keyTable[i].vk == vk)
				return s_keyTable[i].key;
		}
		return EModKey::Unknown;
	}

	const char* ModKeyToName(EModKey key)
	{
		uint32_t idx = static_cast<uint32_t>(key);
		if (static_cast<int>(idx) >= TABLE_SIZE)
			return nullptr;
		return s_keyTable[idx].name;
	}

	EModKey NameToModKey(const char* name)
	{
		if (!name || !*name)
			return EModKey::Unknown;

		// Case-insensitive compare helper
		auto iequal = [](const char* a, const char* b) -> bool
		{
			while (*a && *b)
			{
				if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b)))
					return false;
				++a;
				++b;
			}
			return *a == '\0' && *b == '\0';
		};

		for (int i = 0; i < TABLE_SIZE; ++i)
		{
			if (iequal(s_keyTable[i].name, name))
				return s_keyTable[i].key;
		}
		return EModKey::Unknown;
	}

	// -----------------------------------------------------------------------
	// Combo string helpers (must precede registration functions that call them)
	// -----------------------------------------------------------------------

	// Parse "Ctrl+Shift+F5" into mods + base key.
	// Returns EModKey::Unknown on parse failure.
	static EModKey ParseComboString(const char* combo, EModKeyModifiers& outMods)
	{
		outMods = EModKeyMod_None;
		if (!combo || !*combo)
			return EModKey::Unknown;

		char buf[128];
		strncpy_s(buf, combo, _TRUNCATE);

		EModKey baseKey = EModKey::Unknown;
		char* ctx = nullptr;
		char* token = strtok_s(buf, "+", &ctx);
		while (token)
		{
			while (*token == ' ') ++token;
			size_t len = strlen(token);
			while (len > 0 && token[len - 1] == ' ') token[--len] = '\0';

			auto ieq = [](const char* a, const char* b) -> bool
			{
				while (*a && *b)
				{
					if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
						return false;
					++a; ++b;
				}
				return *a == '\0' && *b == '\0';
			};

			if (ieq(token, "ctrl") || ieq(token, "control"))
				outMods = static_cast<EModKeyModifiers>(outMods | EModKeyMod_Ctrl);
			else if (ieq(token, "shift"))
				outMods = static_cast<EModKeyModifiers>(outMods | EModKeyMod_Shift);
			else if (ieq(token, "alt"))
				outMods = static_cast<EModKeyModifiers>(outMods | EModKeyMod_Alt);
			else
			{
				EModKey k = NameToModKey(token);
				if (k != EModKey::Unknown)
					baseKey = k;
			}
			token = strtok_s(nullptr, "+", &ctx);
		}
		return baseKey;
	}

	const char* FormatComboString(EModKey key, EModKeyModifiers mods, char* outBuf, size_t outLen)
	{
		const char* keyName = ModKeyToName(key);
		if (!keyName) keyName = "Unknown";

		if (mods == EModKeyMod_None)
		{
			strncpy_s(outBuf, outLen, keyName, _TRUNCATE);
			return outBuf;
		}

		char tmp[128] = {};
		if (mods & EModKeyMod_Ctrl)  { if (tmp[0]) strncat_s(tmp, "+", _TRUNCATE); strncat_s(tmp, "Ctrl",  _TRUNCATE); }
		if (mods & EModKeyMod_Shift) { if (tmp[0]) strncat_s(tmp, "+", _TRUNCATE); strncat_s(tmp, "Shift", _TRUNCATE); }
		if (mods & EModKeyMod_Alt)   { if (tmp[0]) strncat_s(tmp, "+", _TRUNCATE); strncat_s(tmp, "Alt",   _TRUNCATE); }
		strncat_s(tmp, "+", _TRUNCATE);
		strncat_s(tmp, keyName, _TRUNCATE);

		strncpy_s(outBuf, outLen, tmp, _TRUNCATE);
		return outBuf;
	}

	// -----------------------------------------------------------------------
	// Registration
	// -----------------------------------------------------------------------
	void RegisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybind: null callback ignored");
			return;
		}
		if (key == EModKey::Unknown)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybind: EModKey::Unknown ignored");
			return;
		}

		std::lock_guard<std::mutex> lock(s_mutex);

		// Prevent duplicate registration of the same callback for the same key+event
		for (auto& e : s_callbacks)
		{
			if (e.key == key && e.event == event && e.callback == callback)
				return;
		}

		s_callbacks.push_back({key, event, callback});

		const char* keyName = ModKeyToName(key);
		ModLoaderLogger::LogDebug(L"[KeybindRegistry] Registered: key=%S event=%u (total=%zu)",
		                          keyName ? keyName : "?",
		                          static_cast<unsigned>(event),
		                          s_callbacks.size());
	}

	void UnregisterKeybind(EModKey key, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!callback) return;

		std::lock_guard<std::mutex> lock(s_mutex);
		auto it = std::remove_if(s_callbacks.begin(), s_callbacks.end(),
		                         [&](const CallbackEntry& e)
		                         {
			                         return e.key == key && e.event == event && e.callback == callback;
		                         });
		s_callbacks.erase(it, s_callbacks.end());
	}

	void RegisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!combo || !*combo)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindByName: null/empty string ignored");
			return;
		}
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindByName: null callback ignored");
			return;
		}

		EModKeyModifiers mods = EModKeyMod_None;
		EModKey key = ParseComboString(combo, mods);
		if (key == EModKey::Unknown)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindByName: could not parse '%S'", combo);
			return;
		}

		// All by-name registrations go into s_namedCombos so UpdateKeybindByName
		// can always locate and patch them when the user rebinds via the config UI.
		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto& e : s_namedCombos)
		{
			if (e.key == key && e.mods == mods && e.event == event && e.callback == callback)
				return; // duplicate
		}

		NamedComboEntry entry = {};
		entry.key      = key;
		entry.mods     = mods;
		entry.event    = event;
		entry.callback = callback;
		strncpy_s(entry.comboStr, combo, _TRUNCATE);
		s_namedCombos.push_back(entry);

		ModLoaderLogger::LogDebug(L"[KeybindRegistry] Registered named bind: combo=%S event=%u (total=%zu)",
		                          combo, static_cast<unsigned>(event), s_namedCombos.size());
	}

	void UnregisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!combo || !*combo || !callback) return;

		EModKeyModifiers mods = EModKeyMod_None;
		EModKey key = ParseComboString(combo, mods);
		if (key == EModKey::Unknown) return;

		std::lock_guard<std::mutex> lock(s_mutex);
		auto it = std::remove_if(s_namedCombos.begin(), s_namedCombos.end(),
		                         [&](const NamedComboEntry& e)
		                         {
			                         return e.key == key && e.mods == mods &&
			                                e.event == event && e.callback == callback;
		                         });
		s_namedCombos.erase(it, s_namedCombos.end());
	}

	void UpdateKeybindByName(const char* pluginName, const char* oldCombo, const char* newCombo)
	{
		if (!oldCombo || !newCombo || !*newCombo) return;

		EModKeyModifiers newMods = EModKeyMod_None;
		EModKey newKey = ParseComboString(newCombo, newMods);
		if (newKey == EModKey::Unknown)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] UpdateKeybindByName: could not parse new combo '%S'", newCombo);
			return;
		}

		std::lock_guard<std::mutex> lock(s_mutex);
		int updated = 0;
		for (auto& e : s_namedCombos)
		{
			if (strcmp(e.comboStr, oldCombo) != 0) continue;

			e.key  = newKey;
			e.mods = newMods;
			strncpy_s(e.comboStr, newCombo, _TRUNCATE);
			++updated;
		}

		if (updated > 0)
			ModLoaderLogger::LogDebug(L"[KeybindRegistry] Live-rebound %d registration(s) for plugin=%S: %S -> %S",
			                          updated, pluginName ? pluginName : "?", oldCombo, newCombo);
	}

	// -----------------------------------------------------------------------
	// Modifier sampling
	// -----------------------------------------------------------------------
	EModKeyModifiers SampleCurrentModifiers()
	{
		EModKeyModifiers mods = EModKeyMod_None;
		if ((GetAsyncKeyState(VK_LCONTROL) | GetAsyncKeyState(VK_RCONTROL)) & 0x8000)
			mods = static_cast<EModKeyModifiers>(mods | EModKeyMod_Ctrl);
		if ((GetAsyncKeyState(VK_LSHIFT) | GetAsyncKeyState(VK_RSHIFT)) & 0x8000)
			mods = static_cast<EModKeyModifiers>(mods | EModKeyMod_Shift);
		if ((GetAsyncKeyState(VK_LMENU) | GetAsyncKeyState(VK_RMENU)) & 0x8000)
			mods = static_cast<EModKeyModifiers>(mods | EModKeyMod_Alt);
		return mods;
	}

	// -----------------------------------------------------------------------
	// Combo registration (v28)
	// -----------------------------------------------------------------------
	void RegisterKeybindCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindCombo: null callback ignored");
			return;
		}
		if (key == EModKey::Unknown)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindCombo: EModKey::Unknown ignored");
			return;
		}

		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto& e : s_comboCallbacks)
		{
			if (e.key == key && e.mods == mods && e.event == event && e.callback == callback)
				return;
		}
		s_comboCallbacks.push_back({key, mods, event, callback});

		char combo[64];
		FormatComboString(key, mods, combo, sizeof(combo));
		ModLoaderLogger::LogDebug(L"[KeybindRegistry] Registered combo: %S event=%u (total=%zu)",
		                          combo, static_cast<unsigned>(event), s_comboCallbacks.size());
	}

	void UnregisterKeybindCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback)
	{
		if (!callback) return;
		std::lock_guard<std::mutex> lock(s_mutex);
		auto it = std::remove_if(s_comboCallbacks.begin(), s_comboCallbacks.end(),
		                         [&](const ComboCallbackEntry& e)
		                         {
			                         return e.key == key && e.mods == mods &&
			                                e.event == event && e.callback == callback;
		                         });
		s_comboCallbacks.erase(it, s_comboCallbacks.end());
	}

	// -----------------------------------------------------------------------
	// Blocking map
	// -----------------------------------------------------------------------
	void SetComboBlocking(const char* comboStr, bool blocking)
	{
		if (!comboStr || !*comboStr) return;

		// Normalise to a canonical string by round-tripping through Parse/Format.
		EModKeyModifiers mods = EModKeyMod_None;
		EModKey key = ParseComboString(comboStr, mods);
		if (key == EModKey::Unknown) return;

		char canonical[64];
		FormatComboString(key, mods, canonical, sizeof(canonical));

		std::lock_guard<std::mutex> lock(s_mutex);
		if (blocking)
			s_blockingMap[canonical] = true;
		else
			s_blockingMap.erase(canonical);
	}

	bool ShouldBlock(EModKey key, EModKeyModifiers mods)
	{
		if (key == EModKey::Unknown) return false;

		char canonical[64];
		FormatComboString(key, mods, canonical, sizeof(canonical));

		std::lock_guard<std::mutex> lock(s_mutex);
		auto it = s_blockingMap.find(canonical);
		return (it != s_blockingMap.end()) && it->second;
	}

	// -----------------------------------------------------------------------
	// Dispatch
	// -----------------------------------------------------------------------
	void Dispatch(EModKey key, EModKeyEvent event)
	{
		// Snapshot callbacks under lock, then call without lock held
		std::vector<PluginKeybindCallback> toCall;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			for (auto& e : s_callbacks)
			{
				if (e.key == key && e.event == event)
					toCall.push_back(e.callback);
			}
		}

		for (auto cb : toCall)
		{
			if (cb)
			{
				try { cb(key, event); }
				catch (...)
				{
				}
			}
		}
	}

	// -----------------------------------------------------------------------
	// DispatchCombo — fires combo callbacks whose (key, mods, event) match
	// -----------------------------------------------------------------------
	void DispatchCombo(EModKey key, EModKeyModifiers mods, EModKeyEvent event)
	{
		// Snapshot both named-combo and advanced-combo lists under lock.
		std::vector<PluginKeybindCallback>      namedToCall;
		std::vector<PluginKeybindComboCallback> advToCall;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			for (auto& e : s_namedCombos)
			{
				if (e.key != key || e.event != event) continue;
				// Named entries with no mods fire regardless of current modifier state
				// (same behaviour as the old plain RegisterKeybind path).
				// Named entries with mods require an exact match.
				if (e.mods == EModKeyMod_None || e.mods == mods)
					namedToCall.push_back(e.callback);
			}
			for (auto& e : s_comboCallbacks)
			{
				if (e.key == key && e.mods == mods && e.event == event)
					advToCall.push_back(e.callback);
			}
		}

		for (auto cb : namedToCall)
		{
			if (cb) try { cb(key, event); } catch (...) {}
		}
		for (auto cb : advToCall)
		{
			if (cb) try { cb(key, mods, event); } catch (...) {}
		}
	}

	// -----------------------------------------------------------------------
	// GetActiveKeys — returns (EModKey, VK) pairs with at least one callback
	// (simple or combo).
	// -----------------------------------------------------------------------
	std::vector<std::pair<EModKey, int>> GetActiveKeys()
	{
		std::vector<std::pair<EModKey, int>> result;

		auto addIfNew = [&](EModKey k)
		{
			for (auto& r : result)
				if (r.first == k) return;
			int vk = ModKeyToVK(k);
			if (vk != 0)
				result.push_back({k, vk});
		};

		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto& e : s_callbacks)
			addIfNew(e.key);
		for (auto& e : s_namedCombos)
			addIfNew(e.key);
		for (auto& e : s_comboCallbacks)
			addIfNew(e.key);

		return result;
	}
} // namespace Hooks::Input

#endif // MODLOADER_CLIENT_BUILD
