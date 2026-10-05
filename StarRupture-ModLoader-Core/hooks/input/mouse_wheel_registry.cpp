#include "pch.h"
#include "mouse_wheel_registry.h"
#include "logging/logger.h"

#include <windows.h>
#include <windowsx.h>
#include <mutex>
#include <vector>

// Client-only feature -- entire translation unit is a no-op on server/generic builds.
#ifdef MODLOADER_CLIENT_BUILD

namespace Hooks::MouseWheel
{
	struct Entry
	{
		PluginMouseWheelCallback callback;
		void*                    userData;
		HMODULE                  owner;     // module the callback lives in, for ForgetModule
	};

	static std::mutex         s_mutex;
	static std::vector<Entry> s_entries;

	static HMODULE ModuleOf(const void* address)
	{
		HMODULE owner = nullptr;
		if (!address ||
		    !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        static_cast<LPCSTR>(address), &owner))
			return nullptr;
		return owner;
	}

	void Register(PluginMouseWheelCallback callback, void* userData)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[MouseWheel] RegisterMouseWheel: null callback ignored");
			return;
		}

		const HMODULE owner = ModuleOf(reinterpret_cast<const void*>(callback));

		std::lock_guard<std::mutex> lock(s_mutex);
		for (const Entry& e : s_entries)
		{
			if (e.callback == callback && e.userData == userData)
			{
				ModLoaderLogger::LogWarn(L"[MouseWheel] RegisterMouseWheel: callback %p / userData %p "
				                         L"is already registered -- ignored",
				                         reinterpret_cast<const void*>(callback), userData);
				return;
			}
		}
		s_entries.push_back({ callback, userData, owner });
		ModLoaderLogger::LogDebug(L"[MouseWheel] Registered callback %p (%zu total)",
		                          reinterpret_cast<const void*>(callback), s_entries.size());
	}

	void Unregister(PluginMouseWheelCallback callback, void* userData)
	{
		if (!callback) return;

		std::lock_guard<std::mutex> lock(s_mutex);
		for (auto it = s_entries.begin(); it != s_entries.end(); ++it)
		{
			if (it->callback == callback && it->userData == userData)
			{
				s_entries.erase(it);
				ModLoaderLogger::LogDebug(L"[MouseWheel] Unregistered callback %p (%zu left)",
				                          reinterpret_cast<const void*>(callback), s_entries.size());
				return;
			}
		}
		ModLoaderLogger::LogWarn(L"[MouseWheel] UnregisterMouseWheel: callback %p / userData %p "
		                         L"was not registered",
		                         reinterpret_cast<const void*>(callback), userData);
	}

	void ForgetModule(HMODULE module)
	{
		if (!module) return;

		std::lock_guard<std::mutex> lock(s_mutex);
		size_t dropped = 0;
		for (auto it = s_entries.begin(); it != s_entries.end(); )
		{
			if (it->owner == module) { it = s_entries.erase(it); ++dropped; }
			else ++it;
		}
		if (dropped)
			ModLoaderLogger::LogDebug(L"[MouseWheel] Dropped %zu wheel handler(s) of an unloading plugin", dropped);
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_entries.clear();
	}

	int GetRegistrationCount()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		return static_cast<int>(s_entries.size());
	}

	// GetKeyState, not GetAsyncKeyState: it is the state as of the message being
	// processed, so a modifier released between the wheel notch and this
	// WndProc running still reads as held -- which is what the user did.
	static uint32_t SampleModifiers()
	{
		auto down = [](int vk) { return (GetKeyState(vk) & 0x8000) != 0; };

		uint32_t m = PluginWheelMod_None;
		if (down(VK_LCONTROL)) m |= PluginWheelMod_LeftCtrl;
		if (down(VK_RCONTROL)) m |= PluginWheelMod_RightCtrl;
		if (down(VK_LSHIFT))   m |= PluginWheelMod_LeftShift;
		if (down(VK_RSHIFT))   m |= PluginWheelMod_RightShift;
		if (down(VK_LMENU))    m |= PluginWheelMod_LeftAlt;
		if (down(VK_RMENU))    m |= PluginWheelMod_RightAlt;
		if (down(VK_LWIN))     m |= PluginWheelMod_LeftWin;
		if (down(VK_RWIN))     m |= PluginWheelMod_RightWin;
		return m;
	}

	bool Dispatch(UINT msg, WPARAM wParam, LPARAM lParam, bool uiCapturing)
	{
		if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) return false;

		// Snapshot under the lock, call without it: a handler may register or
		// unregister (itself included) from inside the callback.
		std::vector<Entry> toCall;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			if (s_entries.empty()) return false;

			// Same stale-pointer guard as the keybind registry's
			// CallbackStillMapped: ForgetModule covers a normal unload, this
			// covers anything that got past it. Under /EHsc the try/catch below
			// does not catch an access violation, so calling into an unmapped
			// module would take the game down.
			for (auto it = s_entries.begin(); it != s_entries.end(); )
			{
				if (!ModuleOf(reinterpret_cast<const void*>(it->callback)))
				{
					ModLoaderLogger::LogError(
						L"[MouseWheel] Dropping a wheel callback at %p whose module is no longer "
						L"loaded. Calling it would have crashed the game.",
						reinterpret_cast<const void*>(it->callback));
					it = s_entries.erase(it);
					continue;
				}
				toCall.push_back(*it);
				++it;
			}
		}

		const int raw = GET_WHEEL_DELTA_WPARAM(wParam);

		PluginMouseWheelEvent ev{};
		ev.size        = sizeof(PluginMouseWheelEvent);
		ev.rawDelta    = (msg == WM_MOUSEWHEEL)  ? raw : 0;
		ev.rawDeltaH   = (msg == WM_MOUSEHWHEEL) ? raw : 0;
		ev.delta       = static_cast<float>(ev.rawDelta)  / static_cast<float>(WHEEL_DELTA);
		ev.deltaH      = static_cast<float>(ev.rawDeltaH) / static_cast<float>(WHEEL_DELTA);
		ev.modifiers   = SampleModifiers();
		ev.screenX     = GET_X_LPARAM(lParam);   // wheel messages carry screen coords
		ev.screenY     = GET_Y_LPARAM(lParam);
		ev.uiCapturing = uiCapturing;

		for (const Entry& e : toCall)
		{
			bool consumed = false;
			try { consumed = e.callback(&ev, e.userData); }
			catch (...)
			{
				ModLoaderLogger::LogError(L"[MouseWheel] Wheel callback %p threw -- treated as not consumed",
				                          reinterpret_cast<const void*>(e.callback));
			}

			// While a modloader window owns the cursor the game is not getting
			// the wheel regardless, and ImGui must keep scrolling -- so every
			// handler sees the event and nobody gets to consume it.
			if (consumed && !uiCapturing)
				return true;
		}
		return false;
	}
}

#endif // MODLOADER_CLIENT_BUILD
