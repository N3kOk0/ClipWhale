// ---------------------------------------------------------------------------
//  ClipWhale - paste the previous clipboard entry with one hotkey
//
//  Deliberately tiny: no windows, no dialogs, no overlays. Just a tray icon,
//  a global hotkey, and two strings.
//
//    copy a   ->  prev = ""    cur = a
//    copy b   ->  prev = a     cur = b
//    hotkey   ->  pastes a, then puts b back on the clipboard
//
//  Plain Win32 / C++, no third-party dependencies, no threads, no polling.
// ---------------------------------------------------------------------------
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

// ---------------------------------------------------------------------------
//  Identity
// ---------------------------------------------------------------------------
#define APP_NAME        L"ClipWhale"
#define APP_VERSION     L"1.0.1"
#define APP_AUTHOR      L"MaoziMGT"
#define APP_URL         L"https://github.com/N3kOk0/ClipWhale"
#define APP_URL_TEXT    L"github.com/N3kOk0/ClipWhale"
#define WND_CLASS_MAIN  L"ClipWhale.MainWnd"
#define WND_CLASS_SET   L"ClipWhale.SettingsWnd"

// ---------------------------------------------------------------------------
//  Messages / timers / ids
// ---------------------------------------------------------------------------
#define WM_CW_TRAY          (WM_APP + 1)
#define WM_CW_SAVE          (WM_APP + 2)

#define TIMER_SAVE          1
#define TIMER_RESTORE       2
#define TIMER_UNSUPPRESS    3
#define TIMER_TRIM          4

#define HOTKEY_ID_MAIN      1
#define TRAY_UID            1

// A single text entry larger than this is ignored.
#define CAP_TEXT_CHARS   (256u * 1024u)

enum : UINT {
    IDM_PAUSE = 1001,
    IDM_AUTOSTART,
    IDM_SETTINGS,
    IDM_RELOAD,
    IDM_OPENDIR,
    IDM_EXIT
};

// control ids for the settings window
enum : int {
    IDC_ABOUT_ICON = 2001,
    IDC_ABOUT_NAME,
    IDC_ABOUT_AUTHOR,
    IDC_ABOUT_LINK,
    IDC_ABOUT_DESC,
    IDC_ABOUT_SEP,
    IDC_HOTKEY,
    IDC_PASTEKEY,
    IDC_DELAY,
    IDC_RESTORE,
    IDC_WAITRELEASE,
    IDC_CHK_AUTOSTART,
    IDC_CHK_TRIM,
    IDC_BTN_SAVE,
    IDC_BTN_CANCEL,
    IDC_BTN_RESET,
    IDC_BTN_OPENDIR,
    IDC_HINT,
    IDC_THEME,         // appended, so the ids above keep their values
    IDC_LBL_AUTOSTART,
    IDC_LBL_TRIM
};

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
struct Config {
    UINT hotkeyMods = MOD_CONTROL | MOD_ALT;   // the trigger
    UINT hotkeyVk   = 'V';

    // What we synthesise to make the target paste. Ctrl+V covers almost
    // everything; Shift+Insert is the escape hatch for terminals and the odd
    // POSIX-flavoured application.
    UINT pasteMods  = MOD_CONTROL;
    UINT pasteVk    = 'V';

    // Clipboard -> keystroke, and keystroke -> put the old clipboard back.
    int  pasteDelayMs   = 25;
    int  restoreDelayMs = 600;

    // 0 (default) = do not wait at all: SendPasteKeys lifts the hotkey's own
    // modifiers for the duration of the keystroke and presses them back
    // afterwards. Anything higher waits that long for the user to let go first.
    int  waitReleaseMs  = 0;

    bool autostart  = true;
    bool trimWhitespace = false;
    int  themeMode  = 0;         // 0 = follow Windows, 1 = force dark, 2 = force light
    bool useSystemHistory = true;  // read Win+V instead of only our own two entries
};

struct AppState {
    HINSTANCE inst = nullptr;
    HWND      main = nullptr;          // hidden message window
    HWND      settings = nullptr;      // options window, only alive while open

    HICON     iconBig = nullptr;
    HICON     iconSmall = nullptr;
    HICON     iconBigDark = nullptr;      // white glyph, for a dark taskbar
    HICON     iconSmallDark = nullptr;

    Config    cfg;

    // The whole "history": the newest text we saw, and the one before it.
    std::wstring cur;
    std::wstring prev;

    bool      dirty = false;
    bool      paused = false;          // runtime only
    bool      suppressCapture = false;

    UINT      msgTaskbarCreated = 0;
    bool      trayAdded = false;
    bool      firstRun = false;
    bool      verbose = false;
    bool      hotkeyOk = true;
};

extern AppState g;

// ---------------------------------------------------------------------------
//  util.cpp
// ---------------------------------------------------------------------------
int          S(int px);                       // scale a length by the DPI
std::string  WideToUtf8(const std::wstring& s);
std::wstring Utf8ToWide(const std::string& s);
std::wstring AppDataDir();
std::wstring ConfigPath();
std::wstring HistoryPath();
std::wstring LogPath();
std::wstring ExePath();
void         Log(const wchar_t* fmt, ...);
std::wstring FormatHotkey(UINT mods, UINT vk);
std::wstring KeyName(UINT vk);
bool         ParseHotkey(const std::wstring& s, UINT& mods, UINT& vk);
bool         ReadAllBytes(const std::wstring& path, std::vector<uint8_t>& out);
// lastError, if given, receives the Win32 error from whichever call failed.
// It has to be captured inside the function: CloseHandle runs on the way out
// and would clobber GetLastError() before the caller could read it.
bool         WriteAllBytes(const std::wstring& path, const void* data, size_t len,
                           DWORD* lastError = nullptr);
bool         AutoStartIsOn();
void         AutoStartSet(bool on);
bool         IsSystemDarkMode();
bool         IsTaskbarDark();
void         TrimProcessWorkingSet();
void         ConfigLoad();
void         ConfigSave();

// ---------------------------------------------------------------------------
//  darkmode.cpp  (uxtheme.dll ordinals 132/133/135/136)
// ---------------------------------------------------------------------------
bool DarkModeInit();          // call once, before any menu/window exists
bool DarkModeEnabled();
void DarkModeApply(HWND hwnd, bool dark);
void DarkModeApplyTree(HWND parent, bool dark);
bool DarkModeRefresh();       // re-read the system setting, true if changed
bool DarkModeReevaluate();    // recompute from cfg.themeMode + system

// The tray icon has two colourways: a black glyph for a light taskbar and a
// white one for a dark taskbar. Picks by IsTaskbarDark() and pushes it to the
// shell. Safe to call before or after TrayAdd().
void AppUpdateTrayIcon();

// ---------------------------------------------------------------------------
//  store.cpp : the two-entry history and its little file
// ---------------------------------------------------------------------------
void StoreLoad();
void StoreSave(bool force = false);
void StoreMarkDirty();
void StorePush(const std::wstring& text);     // prev = cur; cur = text

// ---------------------------------------------------------------------------
//  clipboard.cpp
// ---------------------------------------------------------------------------
bool ClipboardInit(HWND owner);
void ClipboardShutdown(HWND owner);
void ClipboardCapture();                      // called on WM_CLIPBOARDUPDATE
bool ClipboardSetText(const std::wstring& text);
bool ClipboardSnapshot();                     // grab whatever is there now
// Best effort, not a Win32 transaction: see the note in clipboard.cpp.
// False means the snapshot is still held and the call is worth retrying.
bool ClipboardRestoreSnapshot();
void ClipboardDropSnapshot();
bool WaitModifiersUp(int timeoutMs);         // true if they came up in time
void SendPasteKeys(HWND target);

// ---------------------------------------------------------------------------
//  syshistory.cpp : the Windows clipboard history (Win+V)
//
// The entry before the one currently on the clipboard, or false when the
// history cannot be used - switched off, refused, or shorter than two entries.
// Callers fall back to their own store.
// ---------------------------------------------------------------------------
bool SystemHistoryPrevious(std::wstring& out);

// ---------------------------------------------------------------------------
//  settings.cpp
// ---------------------------------------------------------------------------
bool SettingsRegister(HINSTANCE inst);
void SettingsShow();
void SettingsClose();

// ---------------------------------------------------------------------------
//  main.cpp
// ---------------------------------------------------------------------------
void TrayAdd();
void TrayRemove();
void TrayUpdateTip();
void AppPastePrevious();
void AppTogglePause();
void AppToggleAutoStart();
void AppReloadConfig();
void AppOpenDataDir();
void AppSuspendHotkey(bool suspend);
bool AppApplySettings();      // re-register the hotkey + auto-start from g.cfg
