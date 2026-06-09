#include "splash_window.h"

// ---------------------------------------------------------------------------
// Splash window -- only compiles real code for client builds.
// Server builds get empty stubs so callers don't need #ifdefs.
// ---------------------------------------------------------------------------

#if defined(MODLOADER_CLIENT_BUILD)

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr int   SPLASH_WIDTH  = 520;
static constexpr int   SPLASH_HEIGHT = 185;
static constexpr DWORD BG_COLOR      = RGB(24, 24, 28);
static constexpr DWORD TEXT_COLOR    = RGB(200, 200, 210);
static constexpr DWORD BAR_BG_COLOR  = RGB(50, 50, 58);
static constexpr DWORD BAR_FG_COLOR  = RGB(80, 160, 255);
static constexpr DWORD SUB_BAR_COLOR = RGB(60, 130, 200);
static constexpr DWORD TITLE_COLOR   = RGB(255, 255, 255);

static constexpr int MARGIN      = 16;
static constexpr int TITLE_Y     = 14;
static constexpr int STATUS_Y    = 44;
static constexpr int BAR_Y       = 66;
static constexpr int BAR_HEIGHT  = 18;
static constexpr int BAR_X       = MARGIN;
static constexpr int BAR_WIDTH   = SPLASH_WIDTH - MARGIN * 2;

static constexpr int DIVIDER_Y      = 100;   // horizontal rule between sections
static constexpr int SUB_STATUS_Y   = 120;
static constexpr int SUB_BAR_Y      = 140;
static constexpr int SUB_BAR_HEIGHT = 14;

static constexpr DWORD DIVIDER_COLOR  = RGB(55, 55, 65);
static constexpr DWORD DIM_TEXT_COLOR = RGB(100, 100, 110);

// Close button occupies the same rect as the main progress bar.
static constexpr RECT CLOSE_BTN_RECT = { BAR_X, BAR_Y, BAR_X + BAR_WIDTH, BAR_Y + BAR_HEIGHT };

// ---------------------------------------------------------------------------
// State
//
// g_stateLock guards all readable state so SetSubStatus / SetSubProgress /
// ClearSubBar can be called safely from any thread (e.g. InitAllLoadedPlugins
// runs on the game main thread while the splash owner thread is lingering).
// OnPaint snapshots everything under the lock at the start so it never holds
// the lock while doing GDI work.
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_stateLock;
static bool             g_stateLockInit  = false;
static DWORD            g_splashThreadId = 0;   // thread that owns the HWND

static wchar_t g_statusText[256]    = L"Initializing...";
static float   g_progress           = 0.0f;
static bool    g_errorMode          = false;
static bool    g_showCloseButton    = false;
static wchar_t g_subStatusText[256] = L"";
static float   g_subProgress        = 0.0f;
static HWND    g_hwnd               = nullptr;
static bool    g_classRegistered    = false;

// Request a repaint from any thread.
// If called on the splash thread, update synchronously.
// If called from another thread, post WM_APP and let the splash thread paint.
static void RequestRepaint()
{
	if (!g_hwnd) return;
	InvalidateRect(g_hwnd, nullptr, FALSE);
	if (GetCurrentThreadId() == g_splashThreadId)
	{
		UpdateWindow(g_hwnd);
		// Drain window messages so the splash stays alive.
		MSG msg;
		while (PeekMessageW(&msg, g_hwnd, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	else
	{
		// WM_APP tells the splash thread to call UpdateWindow on its own.
		PostMessage(g_hwnd, WM_APP, 0, 0);
	}
}

static constexpr const wchar_t* CLASS_NAME = L"StarRuptureModLoaderSplash";

// ---------------------------------------------------------------------------
// GDI objects
// ---------------------------------------------------------------------------
static HBRUSH g_bgBrush       = nullptr;
static HBRUSH g_barBgBrush    = nullptr;
static HBRUSH g_barFgBrush    = nullptr;
static HBRUSH g_subBarFgBrush = nullptr;
static HFONT  g_titleFont     = nullptr;
static HFONT  g_bodyFont      = nullptr;
static HFONT  g_smallFont     = nullptr;

static void CreateGdiObjects()
{
	g_bgBrush       = CreateSolidBrush(BG_COLOR);
	g_barBgBrush    = CreateSolidBrush(BAR_BG_COLOR);
	g_barFgBrush    = CreateSolidBrush(BAR_FG_COLOR);
	g_subBarFgBrush = CreateSolidBrush(SUB_BAR_COLOR);

	g_titleFont = CreateFontW(
		-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

	g_bodyFont = CreateFontW(
		-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

	g_smallFont = CreateFontW(
		-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void DestroyGdiObjects()
{
	if (g_bgBrush)       { DeleteObject(g_bgBrush);       g_bgBrush       = nullptr; }
	if (g_barBgBrush)    { DeleteObject(g_barBgBrush);    g_barBgBrush    = nullptr; }
	if (g_barFgBrush)    { DeleteObject(g_barFgBrush);    g_barFgBrush    = nullptr; }
	if (g_subBarFgBrush) { DeleteObject(g_subBarFgBrush); g_subBarFgBrush = nullptr; }
	if (g_titleFont)     { DeleteObject(g_titleFont);     g_titleFont     = nullptr; }
	if (g_bodyFont)      { DeleteObject(g_bodyFont);      g_bodyFont      = nullptr; }
	if (g_smallFont)     { DeleteObject(g_smallFont);     g_smallFont     = nullptr; }
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
static void OnPaint(HWND hwnd)
{
	// Snapshot all shared state before any GDI work so we never hold the lock
	// while calling into the display driver.
	wchar_t statusText[256], subStatusText[256];
	float   progress, subProgress;
	bool    errorMode, showCloseButton;
	{
		EnterCriticalSection(&g_stateLock);
		wcscpy_s(statusText,    g_statusText);
		wcscpy_s(subStatusText, g_subStatusText);
		progress        = g_progress;
		subProgress     = g_subProgress;
		errorMode       = g_errorMode;
		showCloseButton = g_showCloseButton;
		LeaveCriticalSection(&g_stateLock);
	}

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

	// Status text -- word-wrap in error mode so longer messages fit on two lines
	SelectObject(hdc, g_bodyFont);
	SetTextColor(hdc, TEXT_COLOR);
	RECT statusRect = { MARGIN, STATUS_Y, SPLASH_WIDTH - MARGIN, BAR_Y - 4 };
	if (errorMode)
		DrawTextW(hdc, statusText, -1, &statusRect, DT_LEFT | DT_WORDBREAK);
	else
		DrawTextW(hdc, statusText, -1, &statusRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

	if (showCloseButton)
	{
		// Close Game button in place of the progress bar.
		HBRUSH btnBrush = CreateSolidBrush(RGB(160, 40, 40));
		RECT btn = CLOSE_BTN_RECT;
		FillRect(hdc, &btn, btnBrush);
		DeleteObject(btnBrush);

		SelectObject(hdc, g_bodyFont);
		SetTextColor(hdc, RGB(255, 255, 255));
		DrawTextW(hdc, L"Click here to close the game", -1, &btn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	else
	{
		// Progress bar background
		RECT barBg = { BAR_X, BAR_Y, BAR_X + BAR_WIDTH, BAR_Y + BAR_HEIGHT };
		FillRect(hdc, &barBg, g_barBgBrush);

		// Progress bar fill
		float pct = progress;
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

		// Horizontal divider between the main section and the sub section.
		{
			HBRUSH divBrush = CreateSolidBrush(DIVIDER_COLOR);
			RECT divRect = { 0, DIVIDER_Y, SPLASH_WIDTH, DIVIDER_Y + 1 };
			FillRect(hdc, &divRect, divBrush);
			DeleteObject(divBrush);
		}

		if (subStatusText[0] != L'\0')
		{
			// Sub-status label
			SelectObject(hdc, g_smallFont);
			SetTextColor(hdc, TEXT_COLOR);
			RECT subLabelRect = { BAR_X, SUB_STATUS_Y, BAR_X + BAR_WIDTH, SUB_BAR_Y - 2 };
			DrawTextW(hdc, subStatusText, -1, &subLabelRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

			// Sub bar background
			RECT subBg = { BAR_X, SUB_BAR_Y, BAR_X + BAR_WIDTH, SUB_BAR_Y + SUB_BAR_HEIGHT };
			FillRect(hdc, &subBg, g_barBgBrush);

			// Sub bar fill
			float subPct = subProgress;
			if (subPct < 0.0f) subPct = 0.0f;
			if (subPct > 1.0f) subPct = 1.0f;
			int subFill = static_cast<int>(BAR_WIDTH * subPct);
			if (subFill > 0)
			{
				RECT subFg = { BAR_X, SUB_BAR_Y, BAR_X + subFill, SUB_BAR_Y + SUB_BAR_HEIGHT };
				FillRect(hdc, &subFg, g_subBarFgBrush);
			}
		}
		else
		{
			// No active sub-status -- show a centred placeholder in the lower panel.
			SelectObject(hdc, g_smallFont);
			SetTextColor(hdc, DIM_TEXT_COLOR);
			RECT placeholderRect = { BAR_X, DIVIDER_Y + 1, BAR_X + BAR_WIDTH, SPLASH_HEIGHT };
			DrawTextW(hdc, L"No Extra Information", -1, &placeholderRect,
			          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}

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
	{
		// When the Close button is visible, let clicks in its rect fall through
		// as client-area hits so WM_LBUTTONUP fires instead of dragging.
		if (g_showCloseButton)
		{
			POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
			ScreenToClient(hwnd, &pt);
			RECT btn = CLOSE_BTN_RECT;
			if (PtInRect(&btn, pt))
				return HTCLIENT;
		}
		return HTCAPTION;
	}

	case WM_LBUTTONUP:
	{
		if (g_showCloseButton)
		{
			POINT pt = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
			RECT btn = CLOSE_BTN_RECT;
			if (PtInRect(&btn, pt))
				ExitProcess(0);
		}
		return 0;
	}

	case WM_APP:
		// Cross-thread repaint request: state was already written; just paint.
		InvalidateRect(hwnd, nullptr, FALSE);
		UpdateWindow(hwnd);
		return 0;

	case WM_DESTROY:
		return 0;

	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}


// ---------------------------------------------------------------------------
// Public API -- all called on the main thread from DllMain
// ---------------------------------------------------------------------------

void Splash::Show()
{
	if (!g_stateLockInit)
	{
		InitializeCriticalSection(&g_stateLock);
		g_stateLockInit = true;
	}
	g_splashThreadId     = GetCurrentThreadId();
	g_progress           = 0.0f;
	g_errorMode          = false;
	g_showCloseButton    = false;
	g_subProgress        = 0.0f;
	g_subStatusText[0]   = L'\0';
	wcscpy_s(g_statusText, L"Initializing...");

	CreateGdiObjects();

	if (!g_classRegistered)
	{
		WNDCLASSEXW wc    = {};
		wc.cbSize         = sizeof(wc);
		wc.lpfnWndProc    = SplashWndProc;
		wc.hInstance      = GetModuleHandleW(nullptr);
		wc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
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
		{ MSG msg; while (PeekMessageW(&msg, g_hwnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
	}
}

void Splash::SetStatus(const wchar_t* text)
{
	if (!g_hwnd || !text) return;
	EnterCriticalSection(&g_stateLock);
	wcscpy_s(g_statusText, text);
	LeaveCriticalSection(&g_stateLock);
	RequestRepaint();
}

void Splash::SetProgress(float fraction)
{
	if (!g_hwnd) return;
	EnterCriticalSection(&g_stateLock);
	g_progress = fraction;
	LeaveCriticalSection(&g_stateLock);
	RequestRepaint();
}

void Splash::Close()
{
	if (!g_hwnd)
		return;

	DestroyWindow(g_hwnd);
	// Drain any remaining messages for this window.
	MSG msg;
	while (PeekMessageW(&msg, g_hwnd, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	g_hwnd           = nullptr;
	g_splashThreadId = 0;

	DestroyGdiObjects();

	if (g_classRegistered)
	{
		UnregisterClassW(CLASS_NAME, GetModuleHandleW(nullptr));
		g_classRegistered = false;
	}

	if (g_stateLockInit)
	{
		DeleteCriticalSection(&g_stateLock);
		g_stateLockInit = false;
	}
}

bool Splash::IsVisible()
{
	return g_hwnd != nullptr;
}

void Splash::SetSubStatus(const wchar_t* text)
{
	if (!g_hwnd) return;
	EnterCriticalSection(&g_stateLock);
	wcscpy_s(g_subStatusText, text ? text : L"");
	LeaveCriticalSection(&g_stateLock);
	RequestRepaint();
}

void Splash::SetSubProgress(float fraction)
{
	if (!g_hwnd) return;
	EnterCriticalSection(&g_stateLock);
	g_subProgress = fraction;
	LeaveCriticalSection(&g_stateLock);
	RequestRepaint();
}

void Splash::ClearSubBar()
{
	if (!g_hwnd) return;
	EnterCriticalSection(&g_stateLock);
	g_subStatusText[0] = L'\0';
	g_subProgress      = 0.0f;
	LeaveCriticalSection(&g_stateLock);
	RequestRepaint();
}

void Splash::Linger(DWORD ms)
{
	if (!g_hwnd) { Sleep(ms); return; }

	DWORD deadline = GetTickCount() + ms;
	for (;;)
	{
		DWORD now = GetTickCount();
		if (now >= deadline) break;
		DWORD remaining = deadline - now;
		// Wake on new messages or at 50ms intervals so we stay responsive.
		MsgWaitForMultipleObjects(0, nullptr, FALSE, min(remaining, 50ul), QS_ALLINPUT);
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
}

void Splash::SetErrorMode(bool showCloseButton)
{
	if (!g_hwnd) return;

	// Switch bar brush to red -- GDI object swap must be on the splash thread.
	if (g_barFgBrush)
	{
		DeleteObject(g_barFgBrush);
		g_barFgBrush = CreateSolidBrush(RGB(200, 50, 50));
	}

	EnterCriticalSection(&g_stateLock);
	g_progress        = 1.0f;
	g_errorMode       = true;
	g_showCloseButton = showCloseButton;
	LeaveCriticalSection(&g_stateLock);

	RequestRepaint();
}

#else // Server build -- empty stubs

void Splash::Show() {}
bool Splash::IsVisible() { return false; }
void Splash::SetStatus(const wchar_t*) {}
void Splash::SetProgress(float) {}
void Splash::SetErrorMode(bool) {}
void Splash::SetSubStatus(const wchar_t*) {}
void Splash::SetSubProgress(float) {}
void Splash::ClearSubBar() {}
void Splash::Linger(DWORD ms) { Sleep(ms); }
void Splash::Close() {}

#endif // MODLOADER_CLIENT_BUILD
