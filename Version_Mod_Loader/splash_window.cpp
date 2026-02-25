#include "splash_window.h"

// ---------------------------------------------------------------------------
// Splash window — only compiles real code for client builds.
// Server builds get empty stubs so callers don't need #ifdefs.
// ---------------------------------------------------------------------------

#if defined(MODLOADER_CLIENT_BUILD)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr int   SPLASH_WIDTH  = 420;
static constexpr int   SPLASH_HEIGHT = 130;
static constexpr DWORD BG_COLOR      = RGB(24, 24, 28);
static constexpr DWORD TEXT_COLOR    = RGB(200, 200, 210);
static constexpr DWORD BAR_BG_COLOR  = RGB(50, 50, 58);
static constexpr DWORD BAR_FG_COLOR  = RGB(80, 160, 255);
static constexpr DWORD TITLE_COLOR   = RGB(255, 255, 255);

static constexpr int MARGIN     = 16;
static constexpr int TITLE_Y    = MARGIN;
static constexpr int STATUS_Y   = 48;
static constexpr int BAR_Y      = 80;
static constexpr int BAR_HEIGHT = 18;
static constexpr int BAR_X  = MARGIN;
static constexpr int BAR_WIDTH  = SPLASH_WIDTH - MARGIN * 2;

// ---------------------------------------------------------------------------
// State — all accessed on the main thread only (no locks needed)
// ---------------------------------------------------------------------------
static wchar_t g_statusText[256] = L"Initializing...";
static float   g_progress   = 0.0f;
static HWND    g_hwnd    = nullptr;
static bool    g_classRegistered = false;

static constexpr const wchar_t* CLASS_NAME = L"StarRuptureModLoaderSplash";

// ---------------------------------------------------------------------------
// GDI objects
// ---------------------------------------------------------------------------
static HBRUSH g_bgBrush    = nullptr;
static HBRUSH g_barBgBrush = nullptr;
static HBRUSH g_barFgBrush = nullptr;
static HFONT  g_titleFont  = nullptr;
static HFONT  g_bodyFont = nullptr;

static void CreateGdiObjects()
{
	g_bgBrush    = CreateSolidBrush(BG_COLOR);
	g_barBgBrush = CreateSolidBrush(BAR_BG_COLOR);
	g_barFgBrush = CreateSolidBrush(BAR_FG_COLOR);

	g_titleFont = CreateFontW(
		-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

	g_bodyFont = CreateFontW(
		-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void DestroyGdiObjects()
{
	if (g_bgBrush)    { DeleteObject(g_bgBrush);    g_bgBrush    = nullptr; }
	if (g_barBgBrush) { DeleteObject(g_barBgBrush);  g_barBgBrush = nullptr; }
	if (g_barFgBrush) { DeleteObject(g_barFgBrush);  g_barFgBrush = nullptr; }
	if (g_titleFont)  { DeleteObject(g_titleFont);   g_titleFont  = nullptr; }
	if (g_bodyFont)   { DeleteObject(g_bodyFont);    g_bodyFont   = nullptr; }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
static void OnPaint(HWND hwnd)
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);

	RECT rc;
	GetClientRect(hwnd, &rc);
	FillRect(hdc, &rc, g_bgBrush);

	SetBkMode(hdc, TRANSPARENT);

	// Title
	SelectObject(hdc, g_titleFont);
	SetTextColor(hdc, TITLE_COLOR);
	RECT titleRect = { MARGIN, TITLE_Y, SPLASH_WIDTH - MARGIN, STATUS_Y };
	DrawTextW(hdc, L"StarRupture Mod Loader", -1, &titleRect, DT_LEFT | DT_SINGLELINE);

	// Status text
	SelectObject(hdc, g_bodyFont);
	SetTextColor(hdc, TEXT_COLOR);
	RECT statusRect = { MARGIN, STATUS_Y, SPLASH_WIDTH - MARGIN, BAR_Y - 4 };
	DrawTextW(hdc, g_statusText, -1, &statusRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

	// Progress bar background
	RECT barBg = { BAR_X, BAR_Y, BAR_X + BAR_WIDTH, BAR_Y + BAR_HEIGHT };
	FillRect(hdc, &barBg, g_barBgBrush);

	// Progress bar fill
	float pct = g_progress;
	if (pct < 0.0f) pct = 0.0f;
	if (pct > 1.0f) pct = 1.0f;
	int fillWidth = static_cast<int>(BAR_WIDTH * pct);
	if (fillWidth > 0)
	{
		RECT barFg = { BAR_X, BAR_Y, BAR_X + fillWidth, BAR_Y + BAR_HEIGHT };
		FillRect(hdc, &barFg, g_barFgBrush);
	}

	// Percentage text centered on bar
	wchar_t pctText[16];
	swprintf_s(pctText, L"%d%%", static_cast<int>(pct * 100.0f));
	SetTextColor(hdc, RGB(255, 255, 255));
	RECT barTextRect = { BAR_X, BAR_Y, BAR_X + BAR_WIDTH, BAR_Y + BAR_HEIGHT };
	DrawTextW(hdc, pctText, -1, &barTextRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_PAINT:
		OnPaint(hwnd);
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_NCHITTEST:
		return HTCAPTION;

	case WM_DESTROY:
		return 0;

	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

// ---------------------------------------------------------------------------
// Pump any pending messages for the splash window so it stays responsive.
// Called from SetStatus / SetProgress on the main thread.
// ---------------------------------------------------------------------------
static void PumpMessages()
{
	if (!g_hwnd)
		return;

	MSG msg;
	while (PeekMessageW(&msg, g_hwnd, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

// ---------------------------------------------------------------------------
// Public API — all called on the main thread from DllMain
// ---------------------------------------------------------------------------

void Splash::Show()
{
	g_progress = 0.0f;
	wcscpy_s(g_statusText, L"Initializing...");

	CreateGdiObjects();

	if (!g_classRegistered)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize     = sizeof(wc);
		wc.lpfnWndProc   = SplashWndProc;
		wc.hInstance      = GetModuleHandleW(nullptr);
		wc.hCursor    = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName  = CLASS_NAME;
		wc.hbrBackground  = nullptr;

		if (RegisterClassExW(&wc))
			g_classRegistered = true;
	}

	int screenW = GetSystemMetrics(SM_CXSCREEN);
	int screenH = GetSystemMetrics(SM_CYSCREEN);
	int x = (screenW - SPLASH_WIDTH)  / 2;
	int y = (screenH - SPLASH_HEIGHT) / 2;

	g_hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		CLASS_NAME,
		L"Mod Loader",
		WS_POPUP,
		x, y, SPLASH_WIDTH, SPLASH_HEIGHT,
		nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

	if (g_hwnd)
	{
		ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
		UpdateWindow(g_hwnd);
		PumpMessages();
	}
}

void Splash::SetStatus(const wchar_t* text)
{
	if (!g_hwnd || !text)
		return;

	wcscpy_s(g_statusText, text);

	InvalidateRect(g_hwnd, nullptr, FALSE);
	UpdateWindow(g_hwnd);
	PumpMessages();
}

void Splash::SetProgress(float fraction)
{
	if (!g_hwnd)
		return;

	g_progress = fraction;

	InvalidateRect(g_hwnd, nullptr, FALSE);
	UpdateWindow(g_hwnd);
	PumpMessages();
}

void Splash::Close()
{
	if (!g_hwnd)
		return;

	DestroyWindow(g_hwnd);
	PumpMessages();
	g_hwnd = nullptr;

	DestroyGdiObjects();

	if (g_classRegistered)
	{
		UnregisterClassW(CLASS_NAME, GetModuleHandleW(nullptr));
		g_classRegistered = false;
	}
}

#else // Server build — empty stubs

void Splash::Show() {}
void Splash::SetStatus(const wchar_t*) {}
void Splash::SetProgress(float) {}
void Splash::Close() {}

#endif // MODLOADER_CLIENT_BUILD
