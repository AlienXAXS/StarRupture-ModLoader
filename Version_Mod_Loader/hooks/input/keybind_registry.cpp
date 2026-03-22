#include "pch.h"
#include "keybind_registry.h"
#include "logging/logger.h"

#include <windows.h>
#include <mutex>
#include <map>
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

	static std::vector<CallbackEntry> s_callbacks;
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

	void RegisterKeybindByName(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!keyName || !*keyName)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindByName: null/empty name ignored");
			return;
		}

		EModKey key = NameToModKey(keyName);
		if (key == EModKey::Unknown)
		{
			ModLoaderLogger::LogWarn(L"[KeybindRegistry] RegisterKeybindByName: unknown key name '%S'", keyName);
			return;
		}

		RegisterKeybind(key, event, callback);
	}

	void UnregisterKeybindByName(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!keyName || !*keyName || !callback) return;
		EModKey key = NameToModKey(keyName);
		if (key != EModKey::Unknown)
			UnregisterKeybind(key, event, callback);
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
	// GetActiveKeys — returns (EModKey, VK) pairs with at least one callback
	// -----------------------------------------------------------------------
	std::vector<std::pair<EModKey, int>> GetActiveKeys()
	{
		std::vector<std::pair<EModKey, int>> result;

		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto& e : s_callbacks)
		{
			// Add if not already in result
			bool found = false;
			for (auto& r : result)
				if (r.first == e.key)
				{
					found = true;
					break;
				}

			if (!found)
			{
				int vk = ModKeyToVK(e.key);
				if (vk != 0)
					result.push_back({e.key, vk});
			}
		}

		return result;
	}
} // namespace Hooks::Input

#endif // MODLOADER_CLIENT_BUILD
