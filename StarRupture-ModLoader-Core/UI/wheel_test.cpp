#include "pch.h"
#include "wheel_test.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "hooks/hooks_interface.h"
#include "hooks/input/mouse_wheel_registry.h"
#include "logging/logger.h"

#include <windows.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>

namespace UI::WheelTest
{
	// -----------------------------------------------------------------------
	// Live hook
	// -----------------------------------------------------------------------

	enum class Mode : int { Observe = 0, ConsumeCtrl = 1, ConsumeAll = 2 };

	struct Record
	{
		PluginMouseWheelEvent ev;
		bool                  consumed;
	};

	static constexpr int kHistory = 8;

	// Written on the game thread (the WndProc), read on the render thread.
	static std::mutex s_liveMutex;
	static bool       s_liveOn        = false;
	static Mode       s_mode          = Mode::Observe;
	static int        s_total         = 0;
	static int        s_consumed      = 0;
	static int        s_whileCapture  = 0;
	static float      s_accumV        = 0.0f;
	static float      s_accumH        = 0.0f;
	static Record     s_history[kHistory] = {};
	static int        s_historyCount  = 0;     // total ever written; ring index = n % kHistory

	static void FormatMods(uint32_t m, char* out, size_t outSize)
	{
		out[0] = '\0';
		struct { uint32_t bit; const char* name; } names[] = {
			{ PluginWheelMod_LeftCtrl,   "LCtrl"  }, { PluginWheelMod_RightCtrl,  "RCtrl"  },
			{ PluginWheelMod_LeftShift,  "LShift" }, { PluginWheelMod_RightShift, "RShift" },
			{ PluginWheelMod_LeftAlt,    "LAlt"   }, { PluginWheelMod_RightAlt,   "RAlt"   },
			{ PluginWheelMod_LeftWin,    "LWin"   }, { PluginWheelMod_RightWin,   "RWin"   },
		};
		for (const auto& n : names)
		{
			if (!(m & n.bit)) continue;
			if (out[0]) strncat_s(out, outSize, "+", _TRUNCATE);
			strncat_s(out, outSize, n.name, _TRUNCATE);
		}
		if (!out[0]) strncpy_s(out, outSize, "-", _TRUNCATE);
	}

	static bool LiveHandler(const PluginMouseWheelEvent* ev, void* /*userData*/)
	{
		if (!ev) return false;

		std::lock_guard<std::mutex> lock(s_liveMutex);

		bool consume = false;
		switch (s_mode)
		{
		case Mode::ConsumeCtrl: consume = (ev->modifiers & PluginWheelMod_Ctrl) != 0; break;
		case Mode::ConsumeAll:  consume = true;                                         break;
		default:                                                                        break;
		}
		// The registry ignores the return value under capture; record what will
		// actually happen rather than what was asked for.
		const bool effective = consume && !ev->uiCapturing;

		++s_total;
		if (effective)       ++s_consumed;
		if (ev->uiCapturing) ++s_whileCapture;
		s_accumV += ev->delta;
		s_accumH += ev->deltaH;
		s_history[s_historyCount % kHistory] = { *ev, effective };
		++s_historyCount;

		char mods[64];
		FormatMods(ev->modifiers, mods, sizeof(mods));
		ModLoaderLogger::LogMessage(L"[WheelTest] raw=%d rawH=%d mods=%S at=(%d,%d) capturing=%d -> %s",
			ev->rawDelta, ev->rawDeltaH, mods, ev->screenX, ev->screenY,
			ev->uiCapturing ? 1 : 0, effective ? L"CONSUMED" : L"passed");

		return consume;
	}

	static IPluginInputEvents* PluginInput()
	{
		IPluginHooks* hooks = ModLoaderLogger::GetPluginHooks();
		return hooks ? hooks->Input : nullptr;
	}

	static void SetLive(bool on)
	{
		IPluginInputEvents* input = PluginInput();
		if (!input || !input->RegisterMouseWheel || !input->UnregisterMouseWheel)
			return;

		if (on)  input->RegisterMouseWheel(LiveHandler, nullptr);
		else     input->UnregisterMouseWheel(LiveHandler, nullptr);

		std::lock_guard<std::mutex> lock(s_liveMutex);
		s_liveOn = on;
	}

	static void ResetStats()
	{
		std::lock_guard<std::mutex> lock(s_liveMutex);
		s_total = s_consumed = s_whileCapture = 0;
		s_accumV = s_accumH = 0.0f;
		s_historyCount = 0;
	}

	// -----------------------------------------------------------------------
	// Self-test
	//
	// Drives Hooks::MouseWheel::Dispatch directly with fabricated messages, so
	// it needs nobody else's handler in the chain: a plugin's handler would be
	// called with fake events and could consume them ahead of ours. It refuses
	// to run in that case rather than report a misleading failure.
	// -----------------------------------------------------------------------

	struct Probe
	{
		int                   calls   = 0;
		bool                  reply   = false;
		PluginMouseWheelEvent last    = {};
	};
	static Probe s_probeA;
	static Probe s_probeB;

	static bool ProbeHandler(const PluginMouseWheelEvent* ev, void* userData)
	{
		Probe* p = static_cast<Probe*>(userData);
		++p->calls;
		p->last = *ev;
		return p->reply;
	}

	static std::vector<std::string> s_results;
	static int                      s_passed = 0;
	static int                      s_failed = 0;

	static void Check(bool ok, const char* what)
	{
		char line[256];
		snprintf(line, sizeof(line), "%s  %s", ok ? "PASS" : "FAIL", what);
		s_results.emplace_back(line);
		if (ok) ++s_passed; else ++s_failed;
		ModLoaderLogger::LogMessage(L"[WheelTest] %S", line);
	}

	static WPARAM WheelW(int delta)          { return MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(delta))); }
	static LPARAM PointL(int x, int y)       { return MAKELPARAM(static_cast<WORD>(static_cast<short>(x)), static_cast<WORD>(static_cast<short>(y))); }

	static void RunSelfTest()
	{
		s_results.clear();
		s_passed = s_failed = 0;

		IPluginInputEvents* input = PluginInput();
		if (!input || !input->RegisterMouseWheel || !input->UnregisterMouseWheel)
		{
			Check(false, "hooks->Input exposes RegisterMouseWheel / UnregisterMouseWheel");
			return;
		}

		// Take the live hook out of the chain for the duration, put it back after.
		bool wasLive;
		{
			std::lock_guard<std::mutex> lock(s_liveMutex);
			wasLive = s_liveOn;
		}
		if (wasLive) SetLive(false);

		const int baseline = Hooks::MouseWheel::GetRegistrationCount();
		if (baseline != 0)
		{
			char msg[160];
			snprintf(msg, sizeof(msg),
			         "no other wheel handlers registered (found %d -- unload the plugin(s) holding them)",
			         baseline);
			Check(false, msg);
			if (wasLive) SetLive(true);
			return;
		}

		s_probeA = {};
		s_probeB = {};

		// Registration
		input->RegisterMouseWheel(ProbeHandler, &s_probeA);
		Check(Hooks::MouseWheel::GetRegistrationCount() == 1, "register adds one handler");
		input->RegisterMouseWheel(ProbeHandler, &s_probeA);
		Check(Hooks::MouseWheel::GetRegistrationCount() == 1, "registering the same (callback, userData) twice is ignored");

		// Payload, vertical
		s_probeA.reply = false;
		bool consumed = Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(240), PointL(123, -45), false);
		Check(s_probeA.calls == 1, "WM_MOUSEWHEEL reaches the handler");
		Check(!consumed, "handler returning false does not consume");
		Check(s_probeA.last.size == sizeof(PluginMouseWheelEvent), "event.size == sizeof(PluginMouseWheelEvent)");
		Check(s_probeA.last.rawDelta == 240 && s_probeA.last.rawDeltaH == 0, "vertical: rawDelta 240, rawDeltaH 0");
		Check(s_probeA.last.delta == 2.0f && s_probeA.last.deltaH == 0.0f, "vertical: delta 2.0 notches");
		Check(s_probeA.last.screenX == 123 && s_probeA.last.screenY == -45, "screen position decoded, negative Y kept signed");
		Check(!s_probeA.last.uiCapturing, "uiCapturing false outside capture");

		// Payload, horizontal, sub-notch
		Hooks::MouseWheel::Dispatch(WM_MOUSEHWHEEL, WheelW(-30), PointL(0, 0), false);
		Check(s_probeA.last.rawDelta == 0 && s_probeA.last.rawDeltaH == -30, "horizontal: rawDeltaH -30, rawDelta 0");
		Check(s_probeA.last.deltaH == -0.25f, "horizontal: deltaH -0.25 (precision delta not rounded)");

		// Consume rules
		s_probeA.reply = true;
		consumed = Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(-120), PointL(0, 0), false);
		Check(consumed, "handler returning true consumes outside capture");

		const int before = s_probeA.calls;
		consumed = Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(-120), PointL(0, 0), true);
		Check(s_probeA.calls == before + 1 && s_probeA.last.uiCapturing, "delivered under capture with uiCapturing = true");
		Check(!consumed, "consume ignored under capture (ImGui keeps the wheel)");

		// Non-wheel messages are not the registry's business
		const int beforeKey = s_probeA.calls;
		consumed = Hooks::MouseWheel::Dispatch(WM_KEYDOWN, VK_SPACE, 0, false);
		Check(!consumed && s_probeA.calls == beforeKey, "non-wheel message ignored");

		// Chain order: A first, B second
		input->RegisterMouseWheel(ProbeHandler, &s_probeB);
		Check(Hooks::MouseWheel::GetRegistrationCount() == 2, "same callback with different userData is a second registration");

		s_probeA.reply = true;
		s_probeB.calls = 0;
		Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(120), PointL(0, 0), false);
		Check(s_probeB.calls == 0, "a consuming handler stops the chain");

		s_probeA.reply = false;
		s_probeB.reply = true;
		consumed = Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(120), PointL(0, 0), false);
		Check(s_probeB.calls == 1 && consumed, "a passing handler lets the next one run and consume");

		s_probeA.reply = true;
		s_probeB.calls = 0;
		Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(120), PointL(0, 0), true);
		Check(s_probeB.calls == 1, "under capture every handler runs even if one asked to consume");

		// Unregister
		input->UnregisterMouseWheel(ProbeHandler, &s_probeA);
		Check(Hooks::MouseWheel::GetRegistrationCount() == 1, "unregister removes only the matching (callback, userData)");
		const int aCalls = s_probeA.calls;
		Hooks::MouseWheel::Dispatch(WM_MOUSEWHEEL, WheelW(120), PointL(0, 0), false);
		Check(s_probeA.calls == aCalls, "unregistered handler is no longer called");

		// Unload cleanup: what the plugin manager calls before FreeLibrary
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   reinterpret_cast<LPCSTR>(&ProbeHandler), &self);
		Hooks::MouseWheel::ForgetModule(self);
		Check(Hooks::MouseWheel::GetRegistrationCount() == 0, "ForgetModule drops the unloading module's handlers");

		// Belt and braces: leave nothing of ours behind whatever failed above.
		input->UnregisterMouseWheel(ProbeHandler, &s_probeA);
		input->UnregisterMouseWheel(ProbeHandler, &s_probeB);

		if (wasLive) SetLive(true);

		ModLoaderLogger::LogMessage(L"[WheelTest] Self-test: %d passed, %d failed", s_passed, s_failed);
	}

	// -----------------------------------------------------------------------
	// UI
	// -----------------------------------------------------------------------

	static void DrawHistory()
	{
		if (!ImGui::BeginTable("##wheel_hist", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
			return;

		ImGui::TableSetupColumn("raw");
		ImGui::TableSetupColumn("rawH");
		ImGui::TableSetupColumn("mods");
		ImGui::TableSetupColumn("capt");
		ImGui::TableSetupColumn("result");
		ImGui::TableHeadersRow();

		const int n = s_historyCount < kHistory ? s_historyCount : kHistory;
		for (int i = 0; i < n; ++i)
		{
			const Record& r = s_history[(s_historyCount - 1 - i) % kHistory];   // newest first
			char mods[64];
			FormatMods(r.ev.modifiers, mods, sizeof(mods));

			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("%d", r.ev.rawDelta);
			ImGui::TableNextColumn(); ImGui::Text("%d", r.ev.rawDeltaH);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(mods);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ev.uiCapturing ? "yes" : "no");
			ImGui::TableNextColumn();
			if (r.consumed) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "consumed");
			else            ImGui::TextUnformatted("passed");
		}
		ImGui::EndTable();
	}

	// Caller holds s_liveMutex.
	static void DrawStatsLocked()
	{
		static const char* kModeNames[] = { "observe", "consume Ctrl+wheel", "consume all" };
		ImGui::Text("Mode: %s", kModeNames[static_cast<int>(s_mode)]);
		ImGui::Text("Events: %d   consumed: %d   under capture: %d", s_total, s_consumed, s_whileCapture);
		ImGui::Text("Accumulated: V %+.2f   H %+.2f notches", s_accumV, s_accumH);
		DrawHistory();
	}

	void DrawControls()
	{
		ImGui::PushID("wheel_test");

		bool live;
		{
			std::lock_guard<std::mutex> lock(s_liveMutex);
			live = s_liveOn;
		}
		if (ImGui::Checkbox("Mouse wheel live hook", &live))
			SetLive(live);
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Registers a wheel handler through hooks->Input->RegisterMouseWheel and\n"
			                  "shows a readout at the top of the screen that stays up after you close\n"
			                  "this window. Close it to test: while it is open you have capture, so\n"
			                  "every event says capt=yes and nothing can be consumed.\n"
			                  "Each event is also written to modloader.log as [WheelTest].");

		{
			std::lock_guard<std::mutex> lock(s_liveMutex);
			int mode = static_cast<int>(s_mode);
			ImGui::RadioButton("Observe", &mode, 0);                ImGui::SameLine();
			ImGui::RadioButton("Consume Ctrl+wheel", &mode, 1);     ImGui::SameLine();
			ImGui::RadioButton("Consume all", &mode, 2);
			s_mode = static_cast<Mode>(mode);

			if (s_liveOn)
				DrawStatsLocked();
		}

		if (ImGui::Button("Reset wheel stats"))
			ResetStats();
		ImGui::SameLine();
		if (ImGui::Button("Run wheel self-test"))
			RunSelfTest();
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Sends synthetic wheel messages through the dispatcher and checks\n"
			                  "registration, payload decoding, chain order, consume rules and\n"
			                  "unload cleanup. Needs no other plugin to hold a wheel handler.");

		if (!s_results.empty())
		{
			if (s_failed == 0)
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Self-test: all %d checks passed", s_passed);
			else
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Self-test: %d passed, %d FAILED", s_passed, s_failed);

			if (ImGui::TreeNode("Self-test details"))
			{
				for (const std::string& line : s_results)
				{
					if (line.compare(0, 4, "FAIL") == 0)
						ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", line.c_str());
					else
						ImGui::TextUnformatted(line.c_str());
				}
				ImGui::TreePop();
			}
		}

		ImGui::PopID();
	}

	void RenderOverlay()
	{
		std::lock_guard<std::mutex> lock(s_liveMutex);
		if (!s_liveOn) return;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 10.0f),
		                        ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.65f);

		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoInputs     | ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

		if (ImGui::Begin("##wheel_test_overlay", nullptr, flags))
		{
			ImGui::TextDisabled("Mouse wheel test (ModLoader > Settings > Debug)");
			DrawStatsLocked();
		}
		ImGui::End();
	}
}

#endif // MODLOADER_CLIENT_BUILD
