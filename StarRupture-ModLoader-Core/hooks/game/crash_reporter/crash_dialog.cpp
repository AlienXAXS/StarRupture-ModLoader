#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "crash_dialog.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#include <vector>
#include <cstdint>

namespace Hooks::CrashDialog
{
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

    // Opens the file in the default editor; if the file is missing, opens its
    // parent folder instead so the user still lands somewhere useful.
    //
    // Runs ShellExecuteW on its own short-lived thread: the dialog can be
    // running very early in process startup (pattern preflight) on a thread
    // without COM initialized, where a direct ShellExecuteW call can silently
    // defer until the dialog's message loop exits -- the log only appeared
    // after the user clicked through. A dedicated STA thread avoids both
    // problems.
    static DWORD WINAPI OpenLogFileThreadProc(LPVOID param)
    {
        std::wstring* path = static_cast<std::wstring*>(param);

        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        if (GetFileAttributesW(path->c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            ShellExecuteW(nullptr, L"open", path->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else
        {
            std::wstring folder = *path;
            size_t slash = folder.find_last_of(L'\\');
            if (slash != std::wstring::npos)
                folder.resize(slash);
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        if (SUCCEEDED(comInit))
            CoUninitialize();

        delete path;
        return 0;
    }

    static void OpenLogFile(HWND /*owner*/, const std::wstring& path)
    {
        if (path.empty())
            return;

        auto* pathCopy = new std::wstring(path);
        HANDLE thread = CreateThread(nullptr, 0, OpenLogFileThreadProc, pathCopy, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            delete pathCopy; // thread creation failed -- don't leak the copy
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
        constexpr int16_t kDlgH = 262;

        const bool patternMode = (mode == Mode::PatternFailure);

        std::vector<uint8_t> buf;

        DLGTEMPLATE dlg{};
        dlg.style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
        dlg.cdit = patternMode ? 7 : 6;
        dlg.x = 0; dlg.y = 0;
        dlg.cx = kDlgW; dlg.cy = kDlgH;
        AppendData(buf, &dlg, sizeof(dlg));
        AppendWord(buf, 0);   // no menu
        AppendWord(buf, 0);   // default dialog class
        AppendString(buf, patternMode
            ? L"StarRupture ModLoader - Failed to start"
            : L"StarRupture ModLoader - The game has crashed");
        // DS_SETFONT: point size then face name
        AppendWord(buf, 9);
        AppendString(buf, L"Segoe UI");

        // Header explanation text
        AppendControl(buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
            7, 7, kDlgW - 14, 88, 0xFFFF, kClassStatic,
            patternMode ? kPatternHeaderText : kCrashHeaderText);

        // Details label
        AppendControl(buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
            7, 99, kDlgW - 14, 9, 0xFFFF, kClassStatic,
            patternMode
                ? L"Missing scan patterns (select and copy, or use the buttons below):"
                : L"Crash details (select and copy, or use the buttons below):");

        // Read-only multiline edit with the details
        AppendControl(buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            0,
            7, 110, kDlgW - 14, 126, IDC_DETAILS_EDIT, kClassEdit, L"");

        // Bottom button row
        constexpr int16_t kBtnY = kDlgH - 21;
        AppendControl(buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
            7, kBtnY, 70, 14, IDC_COPY_DETAILS, kClassButton, L"Copy Details");
        AppendControl(buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
            81, kBtnY, 88, 14, IDC_OPEN_MODLOADER_LOG, kClassButton, L"Open ModLoader.log");

        if (patternMode)
        {
            // The game hasn't run yet, so there's no fresh StarRupture.log to
            // show -- offer the start/quit choice instead.
            AppendControl(buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                kDlgW - 179, kBtnY, 110, 14, IDC_START_WITHOUT_ML, kClassButton,
                L"Start Game Without ModLoader");
            AppendControl(buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                kDlgW - 61, kBtnY, 54, 14, IDC_QUIT_GAME, kClassButton, L"Quit Game");
        }
        else
        {
            AppendControl(buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                173, kBtnY, 88, 14, IDC_OPEN_GAME_LOG, kClassButton, L"Open StarRupture.log");
            AppendControl(buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
                kDlgW - 61, kBtnY, 54, 14, IDCANCEL, kClassButton, L"Close");
        }

        return buf;
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

            // Make sure the crashed game window doesn't keep us buried
            SetForegroundWindow(hDlg);
            return TRUE;
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
        const std::vector<uint8_t> tmpl = BuildDialogTemplate(mode);

        // Default result if the dialog is dismissed without an explicit choice:
        // fail-safe is "keep going" in both modes (game continues unmodified in
        // pattern mode; nothing left to decide in crash mode).
        DialogParams params{ mode, &detailsText,
            mode == Mode::PatternFailure ? Result::StartWithoutModLoader : Result::Closed };

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
