#include "pch.h"
#include "keybind_registry.h"
#include "hooks/game/text_input_focus/text_input_focus.h"
#include "UI/imgui_backend.h"
#include "logging/logger.h"

#include <windows.h>
#include <mutex>
#include <set>
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

	// Defined below -- resolves EModKey::Tilde's VK for the active layout.
	static int GraveVK();
	bool IsTypingExempt(EModKey key);

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
	// Stores the current combo string so UpdateKeybindByName can find and
	// update registrations in-place when the user rebinds in the config UI.
	struct NamedComboEntry
	{
		EModKey          key;
		EModKeyModifiers mods;
		EModKeyEvent     event;
		PluginKeybindCallback callback;
		char comboStr[64];  // current combo string, e.g. "Ctrl+F5" or "F5"

		// The combo the plugin originally registered with, never rewritten by a
		// rebind. This exists because a live rebind silently breaks unregistration
		// and the plugin has no way to know: it registered "W", the user rebound it
		// to "G", UpdateKeybindByName patched key and comboStr in place, and the
		// plugin still calls UnregisterKeybindByName("W", ...) on shutdown. That
		// matched on the *current* key, found nothing, and left a live pointer into
		// a DLL that was about to be unloaded -- pressing G then crashed the game
		// inside PumpMessages.
		//
		// Keeping the original name means unregistration keeps working with the
		// only name the plugin ever knew, for every plugin, without an interface
		// change or any of them being aware this problem existed.
		char originalComboStr[64];
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
		if (key == EModKey::Tilde)
			return GraveVK();

		uint32_t idx = static_cast<uint32_t>(key);
		if (static_cast<int>(idx) >= TABLE_SIZE)
			return 0;
		return s_keyTable[idx].vk;
	}

	// -----------------------------------------------------------------------
	// Layout-aware "Tilde"
	//
	// EModKey::Tilde means the physical key below Escape. Its virtual-key code
	// is layout-dependent: VK_OEM_3 on US layouts, VK_OEM_8 on UK/IE ones
	// (where VK_OEM_3 is the '/@ key instead). Binding the static table entry
	// alone therefore silently does nothing on a UK keyboard.
	//
	// The scancode is stable across layouts -- 0x29 is always that physical
	// key -- so resolve the VK from the scancode once and use it for Tilde in
	// both directions.
	// -----------------------------------------------------------------------
	// -----------------------------------------------------------------------
	// Typing exemptions
	//
	// Keys registered here still fire their keybind while a text field has
	// focus, exactly like Escape. Intended for toggle keys that must dismiss
	// the very window whose text box is focused -- the developer console's
	// open key being the motivating case: with the console open and its
	// prompt focused, Tilde has to close it rather than type a character.
	//
	// Callers add the key only while that window is open, so the key behaves
	// normally (i.e. types) everywhere else.
	// -----------------------------------------------------------------------
	static std::set<EModKey> s_typingExempt;

	void SetTypingExempt(EModKey key, bool exempt)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (exempt) s_typingExempt.insert(key);
		else        s_typingExempt.erase(key);
	}

	bool IsTypingExempt(EModKey key)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		return s_typingExempt.find(key) != s_typingExempt.end();
	}

	static int GraveVK()
	{
		static const int s_graveVk = []
		{
			const UINT vk = MapVirtualKeyW(0x29 /* scancode below Esc */, MAPVK_VSC_TO_VK);
			return vk ? static_cast<int>(vk) : VK_OEM_3;
		}();
		return s_graveVk;
	}

	// Windows reports the modifier keys through their *generic* virtual codes:
	// a WM_KEYDOWN for either Shift arrives as VK_SHIFT, never VK_LSHIFT. The
	// key table only holds the sided codes, so a plugin binding "LeftShift" or
	// "LeftControl" as a plain key never saw its callback fire -- the lookup
	// simply missed. (Combo strings like "Shift+K" were unaffected: those go
	// through the modifier mask, not through this path.)
	//
	// The sided code is recovered from the message's scancode and extended-key
	// bit, which is exactly what MAPVK_VSC_TO_VK_EX exists for. Ctrl and Alt
	// are simpler -- only the right-hand key sets the extended bit.
	int ResolveSidedVK(WPARAM wParam, LPARAM lParam)
	{
		const UINT vk       = static_cast<UINT>(wParam);
		const UINT scancode = (static_cast<UINT>(lParam) & 0x00FF0000u) >> 16;
		const bool extended = (lParam & 0x01000000) != 0;

		switch (vk)
		{
		case VK_SHIFT:
		{
			const UINT sided = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX);
			return sided != 0 ? static_cast<int>(sided) : VK_LSHIFT;
		}
		case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
		case VK_MENU:    return extended ? VK_RMENU    : VK_LMENU;
		default:         return static_cast<int>(vk);
		}
	}

	EModKey VKToModKey(int vk)
	{
		if (vk == GraveVK())
			return EModKey::Tilde;

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
		strncpy_s(entry.originalComboStr, combo, _TRUNCATE);
		s_namedCombos.push_back(entry);

		ModLoaderLogger::LogDebug(L"[KeybindRegistry] Registered named bind: combo=%S event=%u (total=%zu)",
		                          combo, static_cast<unsigned>(event), s_namedCombos.size());
	}

	void UnregisterKeybindByName(const char* combo, EModKeyEvent event, PluginKeybindCallback callback)
	{
		if (!combo || !*combo || !callback) return;

		EModKeyModifiers mods = EModKeyMod_None;
		EModKey key = ParseComboString(combo, mods);

		// An unparseable combo is not a reason to give up any more: the caller may
		// be passing the name it registered with, and matching on that string does
		// not need the key to resolve.
		std::lock_guard<std::mutex> lock(s_mutex);

		const size_t before = s_namedCombos.size();

		auto it = std::remove_if(s_namedCombos.begin(), s_namedCombos.end(),
		                         [&](const NamedComboEntry& e)
		                         {
			                         if (e.event != event || e.callback != callback)
				                         return false;

			                         // Either the live binding matches what the caller
			                         // asked for, or it is the same registration under
			                         // the name it was created with and has since been
			                         // rebound underneath the caller. Both are theirs.
			                         const bool liveMatch = key != EModKey::Unknown &&
			                                                e.key == key && e.mods == mods;
			                         const bool originalMatch =
			                             strcmp(e.originalComboStr, combo) == 0;

			                         return liveMatch || originalMatch;
		                         });
		s_namedCombos.erase(it, s_namedCombos.end());

		const size_t removed = before - s_namedCombos.size();
		if (removed == 0)
		{
			// Worth a warning rather than silence: the caller believes it has just
			// removed a callback it is about to invalidate, and if nothing was
			// removed then something is about to hold a pointer into an unmapped
			// DLL. The dispatch-time guard will catch it, but this says why.
			ModLoaderLogger::LogWarn(
				L"[KeybindRegistry] UnregisterKeybindByName('%S', event=%u) matched no "
				L"registration for callback %p -- if that plugin is unloading, a stale "
				L"registration may be left behind.",
				combo, static_cast<unsigned>(event), reinterpret_cast<const void*>(callback));
		}
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
	// Registration precheck -- true if any registration (simple, named combo,
	// or advanced combo) exists for the key.  Used to keep the game text-focus
	// scan off the hot path for keys nothing is bound to.
	// -----------------------------------------------------------------------
	static bool HasAnyRegistrationForKey(EModKey key)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto& e : s_callbacks)
			if (e.key == key) return true;
		for (auto& e : s_namedCombos)
			if (e.key == key) return true;
		for (auto& e : s_comboCallbacks)
			if (e.key == key) return true;
		return false;
	}

	// -----------------------------------------------------------------------
	// Dispatch
	// -----------------------------------------------------------------------
	// -----------------------------------------------------------------------
	// Stale-callback guard
	//
	// Every registration here is a raw function pointer into a plugin DLL, and
	// calling one after that DLL has been unloaded is an instant access violation
	// inside PumpMessages -- the crash looks like a modloader bug and takes the
	// whole game down. The `try/catch(...)` around the call sites does not help:
	// under /EHsc an access violation is not a C++ exception, so it sails
	// straight through.
	//
	// Plugins are supposed to unregister on shutdown and normally do. The failure
	// mode that made this necessary is subtler: UpdateKeybindByName rebinds a
	// registration *in place* when the user picks a new key in the config UI, and
	// the plugin is never told. Its own unregister then looks for the old key,
	// finds nothing, and the entry outlives the DLL. Press the new key, crash.
	//
	// So the registry stops trusting its own pointers. A callback address that no
	// longer belongs to a loaded module cannot be valid, whatever any plugin did
	// or failed to do, and this makes the entire crash class impossible rather
	// than fixing one route into it.
	//
	// Checked only for entries about to be called -- one or two per keypress --
	// rather than by sweeping the whole table. GetModuleHandleEx touches the
	// loader lock, and doing that fifty times per keystroke from inside a window
	// procedure is a contention risk with no benefit: an entry nobody presses
	// cannot crash anything, and it gets validated the moment they do.
	// -----------------------------------------------------------------------
	static bool CallbackStillMapped(const void* callback)
	{
		if (!callback) return false;

		HMODULE owner = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       static_cast<LPCSTR>(callback), &owner))
			return false;

		return owner != nullptr;
	}

	// -----------------------------------------------------------------------
	// HasExactModifierMatchLocked -- true when some registration asks for
	// exactly the modifiers currently held.
	//
	// Modifier-agnostic binds (a simple RegisterKeybind, or a by-name bind with
	// no modifier tokens) deliberately fire whatever is held, so a plugin bound
	// to "W" keeps working while the player holds Shift to sprint. That
	// leniency has to yield to a more specific bind though: with both "F7" and
	// "Ctrl+F7" registered, pressing Ctrl+F7 used to fire BOTH, so the plugin
	// that only asked for "F7" saw a keypress aimed at someone else's combo --
	// two windows toggling on one keystroke.
	//
	// So an exact match shadows the agnostic ones. When nothing claims the held
	// modifiers, behaviour is exactly as before.
	//
	// Caller must hold s_mutex.
	// -----------------------------------------------------------------------
	static bool HasExactModifierMatchLocked(EModKey key, EModKeyModifiers mods, EModKeyEvent event)
	{
		if (mods == EModKeyMod_None) return false;

		for (const auto& e : s_namedCombos)
			if (e.key == key && e.event == event && e.mods == mods) return true;
		for (const auto& e : s_comboCallbacks)
			if (e.key == key && e.event == event && e.mods == mods) return true;

		return false;
	}

	void Dispatch(EModKey key, EModKeyModifiers mods, EModKeyEvent event)
	{
		// Snapshot callbacks under lock, then call without lock held
		std::vector<PluginKeybindCallback> toCall;
		{
			std::lock_guard<std::mutex> lock(s_mutex);

			// Simple registrations carry no modifiers of their own, so they are
			// agnostic -- but they still stand down for a bind that claimed the
			// exact modifiers being held.
			if (HasExactModifierMatchLocked(key, mods, event))
				return;

			for (auto it = s_callbacks.begin(); it != s_callbacks.end(); )
			{
				if (it->key != key || it->event != event) { ++it; continue; }

				if (!CallbackStillMapped(reinterpret_cast<const void*>(it->callback)))
				{
					ModLoaderLogger::LogError(
						L"[KeybindRegistry] Dropping a keybind callback at %p whose module is no "
						L"longer loaded -- a plugin was unloaded without unregistering it. "
						L"Calling it would have crashed the game.",
						reinterpret_cast<const void*>(it->callback));
					it = s_callbacks.erase(it);
					continue;
				}

				toCall.push_back(it->callback);
				++it;
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

			// A bind asking for exactly the held modifiers shadows the agnostic
			// ones on the same key -- see HasExactModifierMatchLocked.
			const bool shadowed = HasExactModifierMatchLocked(key, mods, event);

			// Same stale-pointer guard as Dispatch, and this is the list that
			// actually crashed: a rebind patches these entries in place, so a
			// plugin's own unregister can miss them entirely.
			for (auto it = s_namedCombos.begin(); it != s_namedCombos.end(); )
			{
				if (it->key != key || it->event != event) { ++it; continue; }

				// Named entries with mods require an exact match. Entries with no
				// mods fire regardless of current modifier state -- unless a more
				// specific bind has claimed those modifiers.
				const bool exact   = (it->mods == mods);
				const bool agnostic = (it->mods == EModKeyMod_None && !shadowed);
				if (!exact && !agnostic) { ++it; continue; }

				if (!CallbackStillMapped(reinterpret_cast<const void*>(it->callback)))
				{
					ModLoaderLogger::LogError(
						L"[KeybindRegistry] Dropping named keybind '%S' -> callback %p whose module "
						L"is no longer loaded. A plugin was unloaded without unregistering it "
						L"(a live rebind will do this). Calling it would have crashed the game.",
						it->comboStr, reinterpret_cast<const void*>(it->callback));
					it = s_namedCombos.erase(it);
					continue;
				}

				namedToCall.push_back(it->callback);
				++it;
			}

			for (auto it = s_comboCallbacks.begin(); it != s_comboCallbacks.end(); )
			{
				if (it->key != key || it->mods != mods || it->event != event) { ++it; continue; }

				if (!CallbackStillMapped(reinterpret_cast<const void*>(it->callback)))
				{
					ModLoaderLogger::LogError(
						L"[KeybindRegistry] Dropping a combo keybind callback at %p whose module is "
						L"no longer loaded. Calling it would have crashed the game.",
						reinterpret_cast<const void*>(it->callback));
					it = s_comboCallbacks.erase(it);
					continue;
				}

				advToCall.push_back(it->callback);
				++it;
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

	void GetRegistrationCounts(int* outSimple, int* outNamedCombos,
	                           int* outAdvancedCombos, int* outBlocking)
	{
		std::lock_guard<std::mutex> lock(s_mutex);

		if (outSimple)         *outSimple         = static_cast<int>(s_callbacks.size());
		if (outNamedCombos)    *outNamedCombos    = static_cast<int>(s_namedCombos.size());
		if (outAdvancedCombos) *outAdvancedCombos = static_cast<int>(s_comboCallbacks.size());

		if (outBlocking)
		{
			// Absent means non-blocking, and entries are left behind rather than
			// erased when blocking is turned back off, so the count has to be of
			// the entries that are actually true.
			int blocking = 0;
			for (const auto& entry : s_blockingMap)
				if (entry.second) ++blocking;
			*outBlocking = blocking;
		}
	}

	void GetBlockedCombos(char* out, int outSize)
	{
		if (!out || outSize <= 0) return;
		out[0] = '\0';

		std::lock_guard<std::mutex> lock(s_mutex);

		int used = 0;
		for (const auto& entry : s_blockingMap)
		{
			if (!entry.second)
				continue;   // present but turned back off

			const int remaining = outSize - used;
			if (remaining <= 1) break;

			const int written = snprintf(out + used, remaining, "%s%s",
			                             used > 0 ? "," : "", entry.first.c_str());
			if (written < 0 || written >= remaining)
				break;   // truncated; keep what fitted

			used += written;
		}
	}

	bool ProcessWindowMessage(UINT msg, WPARAM wParam, LPARAM lParam)
	{
		EModKey      mk    = EModKey::Unknown;
		EModKeyEvent event = EModKeyEvent::Pressed;

		switch (msg)
		{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (lParam & (1 << 30)) return false; // auto-repeat
			mk    = VKToModKey(ResolveSidedVK(wParam, lParam));
			event = EModKeyEvent::Pressed;
			break;
		case WM_KEYUP:
		case WM_SYSKEYUP:
			mk    = VKToModKey(ResolveSidedVK(wParam, lParam));
			event = EModKeyEvent::Released;
			break;
		case WM_LBUTTONDOWN: mk = EModKey::LeftMouseButton;   event = EModKeyEvent::Pressed;  break;
		case WM_LBUTTONUP:   mk = EModKey::LeftMouseButton;   event = EModKeyEvent::Released; break;
		case WM_RBUTTONDOWN: mk = EModKey::RightMouseButton;  event = EModKeyEvent::Pressed;  break;
		case WM_RBUTTONUP:   mk = EModKey::RightMouseButton;  event = EModKeyEvent::Released; break;
		case WM_MBUTTONDOWN: mk = EModKey::MiddleMouseButton; event = EModKeyEvent::Pressed;  break;
		case WM_MBUTTONUP:   mk = EModKey::MiddleMouseButton; event = EModKeyEvent::Released; break;
		case WM_XBUTTONDOWN:
			mk    = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? EModKey::ThumbMouseButton : EModKey::ThumbMouseButton2;
			event = EModKeyEvent::Pressed;
			break;
		case WM_XBUTTONUP:
			mk    = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? EModKey::ThumbMouseButton : EModKey::ThumbMouseButton2;
			event = EModKeyEvent::Released;
			break;
		default:
			return false;
		}

		if (mk == EModKey::Unknown) return false;

		int vk = ModKeyToVK(mk);
		if (vk == VK_LCONTROL || vk == VK_RCONTROL ||
		    vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
		    vk == VK_LMENU    || vk == VK_RMENU)
			return false;

		// Do not fire keyboard keybinds while the player is typing into a
		// game text field (chat, save name, etc.) or an ImGui text field
		// inside a modloader/plugin panel -- e.g. a plugin bound to "P"
		// must not close its own panel when the player types "Port" into a
		// text box inside that panel. Escape is exempt: it must still be
		// able to close a window while a text field is focused, as are
		// any keys registered via SetTypingExempt.
		// Returning false (not blocked) lets the keystroke flow on to the
		// game/ImGui so it reaches the text box.  Only Pressed is gated:
		// letting Released through means a key held when focus changed does
		// not get stuck down in a plugin's view.  Mouse buttons are unaffected.
		if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
		    mk != EModKey::Escape &&
		    !IsTypingExempt(mk) &&
		    HasAnyRegistrationForKey(mk) &&
		    (Hooks::TextInputFocus::IsGameTextInputFocused() || UI::ImGuiBackend::IsTextInputActive()))
			return false;

		EModKeyModifiers mods = SampleCurrentModifiers();
		Dispatch(mk, mods, event);
		DispatchCombo(mk, mods, event);
		return ShouldBlock(mk, mods);
	}
} // namespace Hooks::Input

#endif // MODLOADER_CLIENT_BUILD
