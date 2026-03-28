#include "hud_overlay.h"
#include "plugin_config.h"
#include "plugin_helpers.h"

#include "Engine_classes.hpp"

#include <cstdio>
#include <cstring>

// Win32 headers define DrawText as a macro (DrawTextW/DrawTextA depending on
// the character-set setting). Undefine it here so calls to SDK::AHUD::DrawText
// resolve to the actual SDK function and not the Win32 macro.
#ifdef DrawText
#undef DrawText
#endif

// ---------------------------------------------------------------------------
// AHUD::PostRender byte pattern (IDA-style, ?? = wildcard).
// Same function targeted by the Compass plugin — ACrHUD does not override it,
// so AHUD::PostRender is always the correct hook point.
// ---------------------------------------------------------------------------
static constexpr const char* PATTERN_AHUD_PostRender =
	"40 55 53 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0";

namespace HudOverlay
{

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------

using PostRender_t = void(__fastcall*)(SDK::AHUD* self);

static PostRender_t g_originalPostRender = nullptr;
static HookHandle   g_hookHandle         = nullptr;

// ---------------------------------------------------------------------------
// Shared timer state — written by SetState() on the game tick,
// read by Hooked_PostRender() on the render tick.
// Both callbacks execute on the same game thread, so no locking is needed.
// ---------------------------------------------------------------------------
static RuptureTimer::TimerState s_state = {};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Format seconds as "M:SS" (e.g. 2550 → "42:30"). Returns "--:--" for
// values < 0 (unknown) and "NOW" when seconds == 0 and isNow is true.
static void FormatTime(char* buf, int bufSize, float seconds, bool nowOnZero = false)
{
	if (seconds < 0.0f)
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "--:--");
		return;
	}
	if (nowOnZero && seconds < 0.5f)
	{
		_snprintf_s(buf, bufSize, _TRUNCATE, "NOW");
		return;
	}
	int total = static_cast<int>(seconds);
	int m     = total / 60;
	int s     = total % 60;
	_snprintf_s(buf, bufSize, _TRUNCATE, "%d:%02d", m, s);
}

// Draw a single line of text with a drop shadow for readability over any
// background. Uses AHUD::DrawText with default engine font (Font = nullptr).
static void DrawLine(SDK::AHUD* hud, float x, float y, float scale, const char* text)
{
	// Convert ASCII to wide string — our text is always plain ASCII digits/letters.
	wchar_t wbuf[128] = {};
	for (int i = 0; text[i] && i < 126; ++i)
		wbuf[i] = static_cast<wchar_t>(text[i]);

	SDK::FString fs(wbuf);
	SDK::FLinearColor shadow{0.0f, 0.0f, 0.0f, 0.75f};
	SDK::FLinearColor white{1.0f, 1.0f, 1.0f, 1.0f};

	// Shadow pass (offset 1 pixel)
	hud->DrawText(fs, shadow, x + 1.0f, y + 1.0f, nullptr, scale, false);
	// Main pass
	hud->DrawText(fs, white,  x,         y,         nullptr, scale, false);
}

// ---------------------------------------------------------------------------
// Position calculation
//
// Anchor names map to 7 points around the screen edge. Text is always
// rendered left-to-right from the calculated origin x/y.
//
// estimatedBlockW — approximate pixel width of the widest text line at
//                   scale 1.0 (default font ~8px/char, longest line ~24 chars).
// lineH           — approximate line height including spacing.
// ---------------------------------------------------------------------------
static constexpr float MARGIN            = 20.0f;
static constexpr float ESTIMATED_CHAR_W = 8.5f;  // pixels per char at scale 1.0
static constexpr int   LONGEST_LINE_CHARS = 24;   // "Next Rupture: --:--" + a bit
static constexpr float LINE_H_BASE       = 20.0f; // px per line at scale 1.0

static void CalcPosition(const char* posName, float scale,
                         float screenW, float screenH,
                         float& outX, float& outY)
{
	const float margin    = MARGIN * scale;
	const float blockW    = ESTIMATED_CHAR_W * LONGEST_LINE_CHARS * scale;
	const float blockH    = LINE_H_BASE * 3.0f * scale; // 3 text lines

	// Default: lower-left
	outX = margin;
	outY = screenH - margin - blockH;

	if (_stricmp(posName, "LowerLeft") == 0)
	{
		outX = margin;
		outY = screenH - margin - blockH;
	}
	else if (_stricmp(posName, "MidLeft") == 0)
	{
		outX = margin;
		outY = screenH * 0.5f - blockH * 0.5f;
	}
	else if (_stricmp(posName, "TopLeft") == 0)
	{
		outX = margin;
		outY = margin;
	}
	else if (_stricmp(posName, "TopMid") == 0)
	{
		outX = screenW * 0.5f - blockW * 0.5f;
		outY = margin;
	}
	else if (_stricmp(posName, "TopRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = margin;
	}
	else if (_stricmp(posName, "MidRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = screenH * 0.5f - blockH * 0.5f;
	}
	else if (_stricmp(posName, "LowerRight") == 0)
	{
		outX = screenW - margin - blockW;
		outY = screenH - margin - blockH;
	}
}

// ---------------------------------------------------------------------------
// PostRender hook
// ---------------------------------------------------------------------------

static void __fastcall Hooked_PostRender(SDK::AHUD* self)
{
	// Always call the original first so the game renders normally.
	if (g_originalPostRender)
		g_originalPostRender(self);

	if (!self || !self->Canvas)
		return;

	if (!RuptureTimerConfig::Config::ShouldShowOverlay())
		return;

	if (!s_state.valid)
		return;

	SDK::UCanvas* canvas = self->Canvas;
	const float screenW = static_cast<float>(canvas->SizeX);
	const float screenH = static_cast<float>(canvas->SizeY);
	if (screenW <= 0.0f || screenH <= 0.0f)
		return;

	const float scale = RuptureTimerConfig::Config::GetOverlayScale();
	const float lineH = LINE_H_BASE * scale;

	float x, y;
	CalcPosition(RuptureTimerConfig::Config::GetOverlayPosition(), scale, screenW, screenH, x, y);

	// --- Line 1: Next Rupture countdown ---
	char nextBuf[16];
	FormatTime(nextBuf, sizeof(nextBuf), s_state.nextRuptureInSeconds, /*nowOnZero=*/true);

	char line1[48];
	_snprintf_s(line1, sizeof(line1), _TRUNCATE, "Next Rupture: %s", nextBuf);
	DrawLine(self, x, y, scale, line1);

	// --- Line 2: Planet status (phase + wave type if active) ---
	char line2[48];
	const bool waveActive = (s_state.waveType != 0); // 0 = None
	if (waveActive)
		_snprintf_s(line2, sizeof(line2), _TRUNCATE, "Planet: %s (%s)", s_state.phaseName, s_state.waveTypeName);
	else
		_snprintf_s(line2, sizeof(line2), _TRUNCATE, "Planet: %s", s_state.phaseName);
	DrawLine(self, x, y + lineH, scale, line2);

	// --- Line 3: Current phase timer ---
	char phaseBuf[16];
	FormatTime(phaseBuf, sizeof(phaseBuf), s_state.phaseRemainingSeconds);

	char line3[48];
	_snprintf_s(line3, sizeof(line3), _TRUNCATE, "Wave Timer: %s", phaseBuf);
	DrawLine(self, x, y + lineH * 2.0f, scale, line3);
}

// ---------------------------------------------------------------------------
// Install / Remove
// ---------------------------------------------------------------------------

bool Install(IPluginScanner* scanner, IPluginHooks* hooks)
{
	if (!scanner || !hooks || !hooks->Hooks)
	{
		LOG_ERROR("[HudOverlay] Install called with null interfaces");
		return false;
	}

	uintptr_t addr = scanner->FindPatternInMainModule(PATTERN_AHUD_PostRender);
	if (!addr)
	{
		LOG_ERROR("[HudOverlay] AHUD::PostRender pattern not found — in-game overlay disabled");
		return false;
	}

	g_hookHandle = hooks->Hooks->Install(addr, (void*)Hooked_PostRender, (void**)&g_originalPostRender);
	if (!g_hookHandle)
	{
		LOG_ERROR("[HudOverlay] Failed to install hook on AHUD::PostRender at 0x%llX",
		          static_cast<unsigned long long>(addr));
		return false;
	}

	LOG_INFO("[HudOverlay] AHUD::PostRender hook installed at 0x%llX", static_cast<unsigned long long>(addr));
	return true;
}

void Remove(IPluginHooks* hooks)
{
	if (g_hookHandle && hooks && hooks->Hooks)
	{
		hooks->Hooks->Remove(g_hookHandle);
		g_hookHandle = nullptr;
		LOG_INFO("[HudOverlay] PostRender hook removed");
	}
	g_originalPostRender = nullptr;
}

void SetState(const RuptureTimer::TimerState& state)
{
	s_state = state;
}

} // namespace HudOverlay
