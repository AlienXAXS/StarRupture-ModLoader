#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "crash_dialog.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "Shell32.lib")
#include <vector>
#include <cstdint>

namespace Hooks::CrashDialog
{
    // ---------------------------------------------------------------------
    // Dark theme -- same palette as the splash window (UI/splash_window.cpp)
    // ---------------------------------------------------------------------
    constexpr COLORREF kBgColor      = RGB(24, 24, 28);
    constexpr COLORREF kTextColor    = RGB(200, 200, 210);
    constexpr COLORREF kTitleColor   = RGB(255, 255, 255);
    constexpr COLORREF kPanelColor   = RGB(50, 50, 58);   // edit box + neutral buttons
    constexpr COLORREF kPanelHot     = RGB(70, 70, 80);
    constexpr COLORREF kAccentColor  = RGB(80, 160, 255); // primary button
    constexpr COLORREF kAccentHot    = RGB(60, 130, 200);
    constexpr COLORREF kDangerColor  = RGB(160, 40, 40);  // Quit Game
    constexpr COLORREF kDangerHot    = RGB(200, 50, 50);
    constexpr COLORREF kBorderColor  = RGB(55, 55, 65);

    // Created on first Show(), kept for the process lifetime (the dialog is
    // shown at most a couple of times, usually right before the process ends).
    static HBRUSH g_bgBrush    = nullptr;
    static HBRUSH g_panelBrush = nullptr;

    // Control IDs
    constexpr int IDC_DETAILS_EDIT      = 1001;
    constexpr int IDC_COPY_DETAILS      = 1002;
    constexpr int IDC_OPEN_MODLOADER_LOG = 1003;
    constexpr int IDC_OPEN_GAME_LOG     = 1004;
    constexpr int IDC_START_WITHOUT_ML  = 1005;
    constexpr int IDC_QUIT_GAME         = 1006;

    struct DialogParams
    {
        Mode mode;
        const std::wstring* details;
        Result result;
    };

    static const wchar_t* kCrashHeaderText =
        L"StarRupture has crashed.\r\n"
        L"\r\n"
        L"IMPORTANT: the ModLoader intercepted this crash, but the ModLoader (or its plugins) "
        L"may NOT be the cause -- the unmodified game can and does crash on its own.\r\n"
        L"\r\n"
        L"Automatic crash reporting to the developers (CreepyJar) has been suppressed: "
        L"CreepyJar do not look at logs or save files from modded clients, so sending this "
        L"report would only add noise. Nothing has been sent anywhere.\r\n"
        L"\r\n"
        L"If you want help, share the details below (and both log files) with the ModLoader "
        L"community instead. To report the crash to CreepyJar, first reproduce it without "
        L"the ModLoader installed.";

    static const wchar_t* kPatternHeaderText =
        L"The ModLoader could not start.\r\n"
        L"\r\n"
        L"One or more required memory scan patterns were not found in the game executable. "
        L"This usually means the game updated and the ModLoader needs an update too.\r\n"
        L"\r\n"
        L"To keep your game safe, the ModLoader has disabled itself: no hooks were installed "
        L"and no plugins will be loaded. You can start the game completely unmodified, or "
        L"quit and wait for a ModLoader update.\r\n"
        L"\r\n"
        L"The missing patterns are listed below and in ModLoader.log.";

    static const wchar_t* kVersionHeaderText =
        L"This ModLoader does not match your version of StarRupture.\r\n"
        L"\r\n"
        L"The ModLoader works by reaching directly into the game's code at very specific "
        L"places. Those places move whenever the game is patched, so each ModLoader release "
        L"is tied to one exact game build. Your game is not that build -- it has most likely "
        L"updated, and a matching ModLoader update is not out yet.\r\n"
        L"\r\n"
        L"Rather than guess and risk corrupting your save or crashing you mid-game, the "
        L"ModLoader has switched itself off: nothing has been hooked and no plugins will be "
        L"loaded. This is deliberate, not a bug.\r\n"
        L"\r\n"
        L"You can play right now with a completely normal, unmodified game, or quit and come "
        L"back once the ModLoader has been updated. Your saves are untouched either way.";

    // -----------------------------------------------------------------------
    // Log file locations
    // -----------------------------------------------------------------------

    // <game exe dir>\ModLoader\Logs\ModLoader.log
    static std::wstring GetModLoaderLogPath()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash)
            return L"";
        *(slash + 1) = L'\0';
        std::wstring result = path;
        result += L"ModLoader\\Logs\\ModLoader.log";
        return result;
    }

    // %LOCALAPPDATA%\StarRupture\Saved\Logs\StarRupture.log
    static std::wstring GetGameLogPath()
    {
        wchar_t localAppData[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData)))
            return L"";
        std::wstring result = localAppData;
        result += L"\\StarRupture\\Saved\\Logs\\StarRupture.log";
        return result;
    }

    // Deliberately uses CreateProcessW rather than ShellExecuteW or a worker
    // thread: in fatal-crash mode every other thread in the process is frozen
    // (see crash_reporter.cpp), and one of them may hold the loader lock or
    // heap lock. A newly created thread can't start (DLL thread-attach needs
    // the loader lock) and ShellExecute's COM machinery can hang the same
    // way. CreateProcessW makes a single kernel call from this thread and
    // needs neither, so it works in both the frozen-crash and the startup
    // preflight contexts.
    static void LaunchDetached(const std::wstring& commandLine)
    {
        // CreateProcessW needs a mutable command-line buffer
        std::wstring cmd = commandLine;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    // Opens an explorer window with the log file pre-selected; if the file
    // doesn't exist yet, opens its parent folder instead.
    static void OpenLogFile(HWND /*owner*/, const std::wstring& path)
    {
        if (path.empty())
            return;

        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            LaunchDetached(L"explorer.exe /select,\"" + path + L"\"");
            return;
        }

        std::wstring folder = path;
        size_t slash = folder.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            folder.resize(slash);
        LaunchDetached(L"explorer.exe \"" + folder + L"\"");
    }

    static void CopyToClipboard(HWND owner, const std::wstring& text)
    {
        if (!OpenClipboard(owner))
            return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem)
        {
            void* dest = GlobalLock(mem);
            if (dest)
            {
                memcpy(dest, text.c_str(), bytes);
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem))
                    GlobalFree(mem);
            }
            else
            {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }

    // -----------------------------------------------------------------------
    // In-memory DLGTEMPLATE construction
    //
    // The classic DLGTEMPLATE stream: header, then per-control DLGITEMTEMPLATE
    // entries, everything DWORD-aligned, strings inline as UTF-16.
    // -----------------------------------------------------------------------

    static void AlignDword(std::vector<uint8_t>& buf)
    {
        while (buf.size() % 4 != 0)
            buf.push_back(0);
    }

    static void AppendData(std::vector<uint8_t>& buf, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        buf.insert(buf.end(), bytes, bytes + size);
    }

    static void AppendWord(std::vector<uint8_t>& buf, uint16_t value)
    {
        AppendData(buf, &value, sizeof(value));
    }

    static void AppendString(std::vector<uint8_t>& buf, const wchar_t* str)
    {
        AppendData(buf, str, (wcslen(str) + 1) * sizeof(wchar_t));
    }

    // Predefined system class ordinals for DLGITEMTEMPLATE
    constexpr uint16_t kClassButton = 0x0080;
    constexpr uint16_t kClassEdit   = 0x0081;
    constexpr uint16_t kClassStatic = 0x0082;

    static void AppendControl(std::vector<uint8_t>& buf, uint32_t style, uint32_t exStyle,
                              int16_t x, int16_t y, int16_t cx, int16_t cy,
                              uint16_t id, uint16_t classOrdinal, const wchar_t* text)
    {
        AlignDword(buf);
        DLGITEMTEMPLATE item{};
        item.style = style;
        item.dwExtendedStyle = exStyle;
        item.x = x; item.y = y; item.cx = cx; item.cy = cy;
        item.id = id;
        AppendData(buf, &item, sizeof(item));
        AppendWord(buf, 0xFFFF);          // class: ordinal follows
        AppendWord(buf, classOrdinal);
        AppendString(buf, text);          // window text
        AppendWord(buf, 0);               // no creation data
    }

    static std::vector<uint8_t> BuildDialogTemplate(Mode mode)
    {
        // Dialog layout in dialog units. 4 horizontal DLUs ~ avg char width,
        // 8 vertical DLUs ~ char height, so this is roughly 700x430 px at 96 DPI.
        constexpr int16_t kDlgW = 380;

        const bool patternMode = (mode == Mode::PatternFailure);
        const bool versionMode = (mode == Mode::VersionMismatch);
        // Both startup-failure modes end with the same start/quit choice.
        const bool choiceMode  = patternMode || versionMode;

        // The version explanation is longer than the other two and its details
        // box only holds a couple of lines, so it trades edit height for header
        // height. Everything below follows from those two numbers.
        const int16_t headerH = versionMode ? 124 : 88;
        const int16_t labelY  = static_cast<int16_t>(7 + headerH + 4);
        const int16_t editY   = static_cast<int16_t>(labelY + 11);
        const int16_t editH   = versionMode ? 44 : 126;
        const int16_t kDlgH   = static_cast<int16_t>(editY + editH + 26);

        std::vector<uint8_t> buf;

        DLGTEMPLATE dlg{};
        dlg.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        // Every mode has 7 controls: header, label, details edit, and a
        // four-button row (crash: Copy/ML log/SR log/Close; pattern and
        // version: Copy/ML log/Start/Quit). An undercount here silently drops
        // the trailing controls from the template.
        dlg.cdit = 7;
        dlg.x = 0; dlg.y = 0;
        dlg.cx = kDlgW; dlg.cy = kDlgH;
        AppendData(buf, &dlg, sizeof(dlg));
        AppendWord(buf, 0);   // no menu
        AppendWord(buf, 0);   // default dialog class
        AppendString(buf, versionMode
            ? L"StarRupture ModLoader - Unsupported game version"
            : patternMode
                ? L"StarRupture ModLoader - Failed to start"
                : L"StarRupture ModLoader - The game has crashed");
        // DS_SETFONT: point size then face name
        AppendWord(buf, 9);
        AppendString(buf, L"Segoe UI");

        // Header explanation text
        AppendControl(buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
            7, 7, kDlgW - 14, headerH, 0xFFFF, kClassStatic,
            versionMode ? kVersionHeaderText
                        : patternMode ? kPatternHeaderText : kCrashHeaderText);

        // Details label
        AppendControl(buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
            7, labelY, kDlgW - 14, 9, 0xFFFF, kClassStatic,
            versionMode
                ? L"Version details (select and copy, or use the buttons below):"
                : patternMode
                    ? L"Missing scan patterns (select and copy, or use the buttons below):"
                    : L"Crash details (select and copy, or use the buttons below):");

        // Read-only multiline edit with the details
        AppendControl(buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0,
            7, editY, kDlgW - 14, editH, IDC_DETAILS_EDIT, kClassEdit, L"");

        // Bottom button row -- all owner-drawn so they follow the dark theme
        constexpr uint32_t kBtnStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
        const int16_t kBtnY = static_cast<int16_t>(kDlgH - 21);
        AppendControl(buf, kBtnStyle, 0,
            7, kBtnY, 70, 14, IDC_COPY_DETAILS, kClassButton, L"Copy Details");
        AppendControl(buf, kBtnStyle, 0,
            81, kBtnY, 88, 14, IDC_OPEN_MODLOADER_LOG, kClassButton, L"Open ModLoader.log");

        if (choiceMode)
        {
            // The game hasn't run yet, so there's no fresh StarRupture.log to
            // show -- offer the start/quit choice instead.
            AppendControl(buf, kBtnStyle, 0,
                kDlgW - 179, kBtnY, 110, 14, IDC_START_WITHOUT_ML, kClassButton,
                L"Start Game Without ModLoader");
            AppendControl(buf, kBtnStyle, 0,
                kDlgW - 61, kBtnY, 54, 14, IDC_QUIT_GAME, kClassButton, L"Quit Game");
        }
        else
        {
            AppendControl(buf, kBtnStyle, 0,
                173, kBtnY, 88, 14, IDC_OPEN_GAME_LOG, kClassButton, L"Open StarRupture.log");
            AppendControl(buf, kBtnStyle, 0,
                kDlgW - 61, kBtnY, 54, 14, IDCANCEL, kClassButton, L"Close");
        }

        return buf;
    }

    // -----------------------------------------------------------------------
    // Dark theming helpers
    // -----------------------------------------------------------------------

    static void EnsureThemeBrushes()
    {
        if (!g_bgBrush)
            g_bgBrush = CreateSolidBrush(kBgColor);
        if (!g_panelBrush)
            g_panelBrush = CreateSolidBrush(kPanelColor);
    }

    // Ask DWM for a dark title bar. Resolved dynamically -- the modloader IS
    // a dwmapi.dll proxy, so linking the import statically would be circular;
    // GetModuleHandle finds the already-loaded (proxied) dwmapi and the call
    // forwards through to the real one.
    static void EnableDarkTitleBar(HWND hwnd)
    {
        HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
        if (!dwm)
            dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm)
            return;

        using DwmSetWindowAttribute_t = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto setAttr = reinterpret_cast<DwmSetWindowAttribute_t>(
            GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (!setAttr)
            return;

        constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
        BOOL dark = TRUE;
        setAttr(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
    }

    static void DrawThemedButton(const DRAWITEMSTRUCT* dis)
    {
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;

        COLORREF fill;
        COLORREF text = kTitleColor;
        switch (dis->CtlID)
        {
        case IDC_START_WITHOUT_ML:
        case IDCANCEL:
            fill = pressed ? kAccentHot : kAccentColor;
            break;
        case IDC_QUIT_GAME:
            fill = pressed ? kDangerHot : kDangerColor;
            break;
        default:
            fill = pressed ? kPanelHot : kPanelColor;
            text = kTextColor;
            break;
        }

        HBRUSH fillBrush = CreateSolidBrush(fill);
        FillRect(dis->hDC, &dis->rcItem, fillBrush);
        DeleteObject(fillBrush);

        if (dis->itemState & ODS_FOCUS)
        {
            HBRUSH borderBrush = CreateSolidBrush(kBorderColor);
            FrameRect(dis->hDC, &dis->rcItem, borderBrush);
            DeleteObject(borderBrush);
        }

        wchar_t label[128]{};
        GetWindowTextW(dis->hwndItem, label, 128);

        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, text);
        RECT rc = dis->rcItem;
        DrawTextW(dis->hDC, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // -----------------------------------------------------------------------
    // Dialog procedure
    // -----------------------------------------------------------------------

    static INT_PTR CALLBACK DialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_INITDIALOG:
        {
            auto* params = reinterpret_cast<DialogParams*>(lParam);
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, lParam);
            if (params && params->details)
                SetDlgItemTextW(hDlg, IDC_DETAILS_EDIT, params->details->c_str());

            // Monospace font for the details box so stack frames line up
            HFONT mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            if (mono)
                SendDlgItemMessageW(hDlg, IDC_DETAILS_EDIT, WM_SETFONT,
                    reinterpret_cast<WPARAM>(mono), TRUE);

            EnableDarkTitleBar(hDlg);

            // Make sure the crashed game window doesn't keep us buried
            SetForegroundWindow(hDlg);
            return TRUE;
        }

        // Dark theme: dialog background, static text, and the details edit box.
        // Note: a read-only edit control sends WM_CTLCOLORSTATIC, not
        // WM_CTLCOLOREDIT, so both controls are handled here.
        case WM_CTLCOLORDLG:
            return reinterpret_cast<INT_PTR>(g_bgBrush);

        case WM_CTLCOLORSTATIC:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_DETAILS_EDIT)
            {
                SetTextColor(hdc, kTextColor);
                SetBkColor(hdc, kPanelColor);
                return reinterpret_cast<INT_PTR>(g_panelBrush);
            }
            SetTextColor(hdc, kTextColor);
            SetBkColor(hdc, kBgColor);
            return reinterpret_cast<INT_PTR>(g_bgBrush);
        }

        case WM_DRAWITEM:
        {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_BUTTON)
            {
                DrawThemedButton(dis);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND:
        {
            auto* params = reinterpret_cast<DialogParams*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
            switch (LOWORD(wParam))
            {
            case IDC_COPY_DETAILS:
                if (params && params->details)
                    CopyToClipboard(hDlg, *params->details);
                return TRUE;
            case IDC_OPEN_MODLOADER_LOG:
                OpenLogFile(hDlg, GetModLoaderLogPath());
                return TRUE;
            case IDC_OPEN_GAME_LOG:
                OpenLogFile(hDlg, GetGameLogPath());
                return TRUE;
            case IDC_START_WITHOUT_ML:
                if (params)
                    params->result = Result::StartWithoutModLoader;
                EndDialog(hDlg, 0);
                return TRUE;
            case IDC_QUIT_GAME:
                if (params)
                    params->result = Result::QuitGame;
                EndDialog(hDlg, 0);
                return TRUE;
            case IDCANCEL:
            case IDOK:
                EndDialog(hDlg, 0);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CLOSE:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return FALSE;
    }

    Result Show(Mode mode, const std::wstring& detailsText)
    {
        EnsureThemeBrushes();

        const std::vector<uint8_t> tmpl = BuildDialogTemplate(mode);

        const bool choiceMode =
            (mode == Mode::PatternFailure || mode == Mode::VersionMismatch);

        // Default result if the dialog is dismissed without an explicit choice:
        // fail-safe is "keep going" in the startup-failure modes (game continues
        // unmodified); nothing left to decide in crash mode.
        DialogParams params{ mode, &detailsText,
            choiceMode ? Result::StartWithoutModLoader : Result::Closed };

        const INT_PTR result = DialogBoxIndirectParamW(
            GetModuleHandleW(nullptr),
            reinterpret_cast<const DLGTEMPLATE*>(tmpl.data()),
            nullptr,
            DialogProc,
            reinterpret_cast<LPARAM>(&params));

        if (result == -1)
        {
            // Dialog creation failed (process can be in a bad state); fall back
            // to a plain message box so the user still sees something.
            if (mode == Mode::VersionMismatch)
            {
                const int choice = MessageBoxW(nullptr,
                    L"This ModLoader does not match your version of StarRupture.\n\n"
                    L"Each ModLoader release is tied to one exact game build, because the "
                    L"places it reaches into move whenever the game is patched. Your game "
                    L"has most likely updated ahead of the ModLoader.\n\n"
                    L"Rather than guess and risk your save, the ModLoader has switched itself "
                    L"off: nothing was hooked and no plugins will be loaded. Details were "
                    L"written to ModLoader\\Logs\\ModLoader.log.\n\n"
                    L"Start the game without the ModLoader?",
                    L"StarRupture ModLoader",
                    MB_YESNO | MB_ICONERROR | MB_TASKMODAL);
                return choice == IDNO ? Result::QuitGame : Result::StartWithoutModLoader;
            }

            if (mode == Mode::PatternFailure)
            {
                const int choice = MessageBoxW(nullptr,
                    L"The ModLoader could not start: required memory scan patterns were not "
                    L"found in the game executable (the game likely updated and the ModLoader "
                    L"needs an update too).\n\n"
                    L"No hooks were installed and no plugins will be loaded. Details were "
                    L"written to ModLoader\\Logs\\ModLoader.log.\n\n"
                    L"Start the game without the ModLoader?",
                    L"StarRupture ModLoader",
                    MB_YESNO | MB_ICONERROR | MB_TASKMODAL);
                return choice == IDNO ? Result::QuitGame : Result::StartWithoutModLoader;
            }

            MessageBoxW(nullptr,
                L"StarRupture has crashed.\n\n"
                L"The ModLoader (or its plugins) may NOT be the cause -- the unmodified game "
                L"can crash on its own.\n\n"
                L"Automatic crash reporting is disabled because the ModLoader is installed; "
                L"CreepyJar do not look at logs or saves from modded clients.\n\n"
                L"Details were written to ModLoader\\Logs\\ModLoader.log.",
                L"StarRupture ModLoader",
                MB_OK | MB_ICONERROR | MB_TASKMODAL);
            return Result::Closed;
        }

        return params.result;
    }
}

#endif // MODLOADER_CLIENT_BUILD
