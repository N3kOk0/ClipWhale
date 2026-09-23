// ---------------------------------------------------------------------------
//  main.cpp : hidden message window, tray icon, and the hotkey that pastes the
//  previous clipboard entry.
//
//  No other windows exist. There is nothing to open, nothing to click through.
// ---------------------------------------------------------------------------
#include "common.h"
#include "resource.h"
#include <cwchar>

AppState g;

// Restoring the snapshot can fail while another application holds the clipboard
// open. clipboard.cpp deliberately keeps the snapshot when that happens, so it
// is worth trying again a few times before leaving the user with an empty
// clipboard.
static const int kRestoreTries   = 8;
static const UINT kRestoreRetryMs = 250;
static int s_restoreTries = 0;

// ---------------------------------------------------------------------------
static bool RegisterHotkey() {
    if (!g.main) return false;
    UnregisterHotKey(g.main, HOTKEY_ID_MAIN);
    BOOL ok = RegisterHotKey(g.main, HOTKEY_ID_MAIN,
                             g.cfg.hotkeyMods | MOD_NOREPEAT, g.cfg.hotkeyVk);
    g.hotkeyOk = ok != FALSE;
    Log(L"hotkey %ls -> %ls", FormatHotkey(g.cfg.hotkeyMods, g.cfg.hotkeyVk).c_str(),
        g.hotkeyOk ? L"ok" : L"FAILED (already taken?)");
    return g.hotkeyOk;
}

// ---------------------------------------------------------------------------
//  tray
// ---------------------------------------------------------------------------
static NOTIFYICONDATAW MakeNid() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.main;
    nid.uID = TRAY_UID;
    return nid;
}

void TrayUpdateTip() {
    if (!g.trayAdded || !g.main) return;
    NOTIFYICONDATAW nid = MakeNid();
    nid.uFlags = NIF_TIP;

    std::wstring tip = APP_NAME;
    tip += L"\n";
    tip += g.paused ? L"已暂停" : L"记录中";
    tip += L"  ·  ";
    tip += FormatHotkey(g.cfg.hotkeyMods, g.cfg.hotkeyVk);
    tip += L" 粘贴上一条";
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// The tray lives on the taskbar, which follows SystemUsesLightTheme - a
// different setting from the one our windows use. A black glyph on a dark
// taskbar is invisible, so there are two colourways and we pick by that flag.
void AppUpdateTrayIcon() {
    if (!g.trayAdded || !g.main) return;
    HICON icon = IsTaskbarDark() ? (g.iconSmallDark ? g.iconSmallDark : g.iconSmall)
                                 : g.iconSmall;
    if (!icon) return;

    NOTIFYICONDATAW nid = MakeNid();
    nid.uFlags = NIF_ICON;
    nid.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    Log(L"tray icon: %ls glyph", IsTaskbarDark() ? L"white" : L"black");
}

void TrayAdd() {
    NOTIFYICONDATAW nid = MakeNid();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_CW_TRAY;
    nid.hIcon = IsTaskbarDark() && g.iconSmallDark ? g.iconSmallDark
              : (g.iconSmall ? g.iconSmall : LoadIconW(nullptr, IDI_APPLICATION));
    lstrcpynW(nid.szTip, APP_NAME, ARRAYSIZE(nid.szTip));

    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (g.trayAdded) TrayUpdateTip();
    Log(L"tray added: %d (taskbar dark=%d)", (int)g.trayAdded, IsTaskbarDark() ? 1 : 0);
}

// A balloon instead of a modal box: nothing may block the message loop, or the
// hotkey would stop working until someone clicks OK.
static void TrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g.trayAdded) return;
    NOTIFYICONDATAW nid = MakeNid();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title.c_str(), ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text.c_str(), ARRAYSIZE(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayRemove() {
    if (!g.trayAdded) return;
    NOTIFYICONDATAW nid = MakeNid();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g.trayAdded = false;
}

// Menu ids double as WM_COMMAND ids, so the tray menu and any programmatic
// caller (including a test harness) share one code path.
static void RunCommand(UINT cmd) {
    switch (cmd) {
    case IDM_PAUSE:     AppTogglePause();     break;
    case IDM_AUTOSTART: AppToggleAutoStart(); break;
    case IDM_SETTINGS:  SettingsShow();       break;
    case IDM_RELOAD:    AppReloadConfig();    break;
    case IDM_OPENDIR:   AppOpenDataDir();     break;
    case IDM_EXIT:      if (g.main) PostMessageW(g.main, WM_CLOSE, 0, 0); break;
    default: break;
    }
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING | (g.paused ? MF_CHECKED : 0u), IDM_PAUSE, L"暂停记录");
    AppendMenuW(m, MF_STRING | (g.cfg.autostart ? MF_CHECKED : 0u), IDM_AUTOSTART, L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(m, MF_STRING, IDM_RELOAD,  L"重新载入配置");
    AppendMenuW(m, MF_STRING, IDM_OPENDIR, L"打开数据目录");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT,    L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.main);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, g.main, nullptr);
    PostMessageW(g.main, WM_NULL, 0, 0);
    DestroyMenu(m);
    RunCommand(cmd);
}

// ---------------------------------------------------------------------------
//  Bring `target` forward, but only if it is not already there. No synthetic
//  ALT tap here on purpose - that trick opens menu bars in some applications.
// ---------------------------------------------------------------------------
static void EnsureForeground(HWND target) {
    if (!target || GetForegroundWindow() == target) return;

    HWND fg = GetForegroundWindow();
    DWORD fgT = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myT = GetCurrentThreadId();
    bool attached = false;
    if (fgT && fgT != myT) attached = AttachThreadInput(fgT, myT, TRUE) != FALSE;

    SetForegroundWindow(target);
    BringWindowToTop(target);

    if (attached) AttachThreadInput(fgT, myT, FALSE);
    Sleep(20);
}

// ---------------------------------------------------------------------------
//  The desktop and the taskbar are always happy to be the foreground window,
//  and clicking the tray to reach our own menu lands you there. Injecting a
//  paste into them does nothing except beep, so treat them as "no target".
// ---------------------------------------------------------------------------
static bool IsShellWindow(HWND h) {
    if (!h) return true;

    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));

    return _wcsicmp(cls, L"Progman") == 0 ||               // desktop
           _wcsicmp(cls, L"WorkerW") == 0 ||               // desktop backdrop
           _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||         // taskbar
           _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;  // taskbar, other monitor
}

// ---------------------------------------------------------------------------
//  The whole point of the program.
// ---------------------------------------------------------------------------
void AppPastePrevious() {
    if (g.prev.empty()) {
        Log(L"paste: nothing to paste yet (only one entry seen)");
        return;
    }

    HWND target = GetForegroundWindow();
    if (!target || target == g.main || IsShellWindow(target)) {
        Log(L"paste: no usable target window");
        return;
    }
    Log(L"paste: target %p", (void*)target);

    // 1. Optionally wait for the user to let go of the hotkey's modifiers.
    //    The default (WaitReleaseMs=0) does not wait at all - SendPasteKeys
    //    lifts them for the duration of the keystroke and presses them back
    //    afterwards. That costs nothing and leaves the key state as it was,
    //    instead of making the user sit through their own key release.
    //
    //    Timing out is not fatal and must not be "fixed" by force-releasing the
    //    modifiers here: SendPasteKeys decides what to press back by looking at
    //    what is held when it runs, so releasing first would make it skip the
    //    press-back and leave the logical key state disagreeing with the
    //    physical one.
    if (g.cfg.waitReleaseMs > 0 && !WaitModifiersUp(g.cfg.waitReleaseMs))
        Log(L"paste: modifiers still down; SendPasteKeys will juggle them");
    Sleep(10);

    // 2. The keystroke goes to whatever window has focus, so make sure that is
    //    the window the user was typing in.
    EnsureForeground(target);

    // 3. Swap the clipboard over to the previous entry.
    const std::wstring text = g.prev;
    s_restoreTries = 0;
    ClipboardSnapshot();
    g.suppressCapture = true;
    if (!ClipboardSetText(text)) {
        g.suppressCapture = false;
        ClipboardDropSnapshot();
        Log(L"paste: could not put the entry on the clipboard");
        return;
    }

    // 4. Give the target a moment to notice the new clipboard contents. Some
    //    applications read the clipboard lazily on their own message pump.
    if (g.cfg.pasteDelayMs > 0) Sleep(g.cfg.pasteDelayMs);

    // 5. The actual paste.
    SendPasteKeys(target);

    // 6. Put the old clipboard back so a plain Ctrl+V keeps working.
    if (g.main) {
        UINT delay = (UINT)(g.cfg.restoreDelayMs > 0 ? g.cfg.restoreDelayMs : 1);
        if (!SetTimer(g.main, TIMER_RESTORE, delay, nullptr)) {
            // Without the timer nothing would ever put the snapshot back, and
            // the user's clipboard would keep our temporary entry.
            Log(L"paste: restore timer FAILED (error %lu) - restoring now",
                (unsigned long)GetLastError());
            ClipboardRestoreSnapshot();
            g.suppressCapture = false;
        }
    } else {
        ClipboardRestoreSnapshot();
        g.suppressCapture = false;
    }
}

// ---------------------------------------------------------------------------
void AppTogglePause() {
    g.paused = !g.paused;
    TrayUpdateTip();
}

void AppToggleAutoStart() {
    g.cfg.autostart = !g.cfg.autostart;
    ConfigSave();
    AutoStartSet(g.cfg.autostart);
}

void AppReloadConfig() {
    ConfigLoad();
    AppApplySettings();
}

// The settings window unregisters the hotkey while its capture field has focus;
// this puts it back (and is also how "save" takes effect).
bool AppApplySettings() {
    DarkModeReevaluate();
    bool ok = RegisterHotkey();
    AutoStartSet(g.cfg.autostart);
    TrayUpdateTip();
    return ok;
}

void AppSuspendHotkey(bool suspend) {
    if (!g.main) return;
    if (suspend) {
        UnregisterHotKey(g.main, HOTKEY_ID_MAIN);
        Log(L"hotkey suspended (recording a new one)");
    } else {
        RegisterHotkey();
    }
}

void AppOpenDataDir() {
    ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
//  hidden main window
// ---------------------------------------------------------------------------
static LRESULT CALLBACK MainProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    if (m == g.msgTaskbarCreated && g.msgTaskbarCreated) {
        g.trayAdded = false;
        TrayAdd();
        return 0;
    }

    switch (m) {
    case WM_CLIPBOARDUPDATE:
        ClipboardCapture();
        return 0;

    case WM_HOTKEY:
        if (w == HOTKEY_ID_MAIN) AppPastePrevious();
        return 0;

    case WM_CW_TRAY: {
        UINT ev = LOWORD(l);
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    }

    case WM_COMMAND:
        RunCommand(LOWORD(w));
        return 0;

    // the app theme flipped - re-arm the dark menu and let the settings window
    // (if open) restyle itself
    case WM_SETTINGCHANGE:
        // the taskbar may have flipped too, which is a different flag
        AppUpdateTrayIcon();
        if (DarkModeRefresh() && g.settings)
            SendMessageW(g.settings, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");
        return 0;

    case WM_TIMER:
        if (w == TIMER_SAVE) {
            KillTimer(hwnd, TIMER_SAVE);
            StoreSave(false);
            return 0;
        }
        if (w == TIMER_RESTORE) {
            KillTimer(hwnd, TIMER_RESTORE);
            if (!ClipboardRestoreSnapshot()) {
                // The clipboard was busy (or the formats would not go back).
                // The snapshot survives a failed restore on purpose, so retry a
                // few times before leaving the user with an empty clipboard.
                ++s_restoreTries;
                if (s_restoreTries < kRestoreTries) {
                    Log(L"paste: restore attempt %d/%d failed, retrying in %u ms",
                        s_restoreTries, kRestoreTries, kRestoreRetryMs);
                    SetTimer(hwnd, TIMER_RESTORE, kRestoreRetryMs, nullptr);
                    return 0;
                }
                Log(L"paste: giving up on the restore after %d attempts", s_restoreTries);
                ClipboardDropSnapshot();
            } else if (s_restoreTries > 0) {
                Log(L"paste: restore succeeded on attempt %d", s_restoreTries + 1);
            }
            s_restoreTries = 0;
            g.suppressCapture = false;
            TrimProcessWorkingSet();
            return 0;
        }
        if (w == TIMER_UNSUPPRESS) {
            KillTimer(hwnd, TIMER_UNSUPPRESS);
            g.suppressCapture = false;
            return 0;
        }
        break;

    case WM_QUERYENDSESSION:
        StoreSave(true);
        return TRUE;

    case WM_ENDSESSION:
        if (w) StoreSave(true);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    // A paste may still be in flight, and the timer that would put the snapshot
    // back is not going to survive the window. WM_DESTROY is reached by every
    // DestroyWindow path, so that is where the restore belongs.
    //
    // The handle is still usable as a clipboard owner while WM_DESTROY runs,
    // but that is an empirical result on current Windows rather than a
    // documented guarantee - the docs only promise the window is off-screen by
    // then. If OpenClipboard ever starts failing here, move this into WM_CLOSE,
    // which runs before the window is destroyed at all.
    case WM_DESTROY:
        if (g.suppressCapture) {
            KillTimer(hwnd, TIMER_RESTORE);
            ClipboardRestoreSnapshot();
            g.suppressCapture = false;
        }
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, FALSE, L"ClipWhale.SingleInstance.v1");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mtx);
        // Worth saying out loud: otherwise "I replaced the exe and it still
        // behaves like the old build" is an easy trap to fall into.
        MessageBoxW(nullptr,
            L"ClipWhale 已经在运行了（就是托盘里那个）。\n\n"
            L"如果你刚替换过 exe 想测试新版本，请先右键托盘图标 → 退出，再重新启动；\n"
            L"否则你按快捷键触发的仍然是旧版本。",
            APP_NAME, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return 0;
    }

    g.inst = inst;
    const wchar_t* cmdline = GetCommandLineW();
    g.verbose = cmdline && wcsstr(cmdline, L"--verbose") != nullptr;
    if (g.verbose) Log(L"--- ClipWhale %ls starting ---", APP_VERSION);

    ConfigLoad();
    StoreLoad();

    // Must run before any window or menu exists: it is what makes user32 draw
    // our TrackPopupMenu dark.
    DarkModeInit();

    // themed standard controls for the settings window
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    g.iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g.iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g.iconSmall) g.iconSmall = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g.iconBig) g.iconBig = g.iconSmall;

    g.iconBigDark = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON_DARK), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g.iconSmallDark = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON_DARK), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g.iconBigDark) g.iconBigDark = g.iconBig;
    if (!g.iconSmallDark) g.iconSmallDark = g.iconSmall;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = g.iconBig;
    wc.hIconSm = g.iconSmall;
    wc.lpszClassName = WND_CLASS_MAIN;
    if (!RegisterClassExW(&wc)) return 1;
    if (!SettingsRegister(inst)) return 1;

    g.main = CreateWindowExW(0, WND_CLASS_MAIN, APP_NAME, WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g.main) return 1;

    // Must happen after the window exists: this is what makes user32 draw our
    // TrackPopupMenu (the tray menu) dark. Doing it in DarkModeInit() would be
    // a no-op because g.main is still null at that point.
    DarkModeApply(g.main, DarkModeEnabled());

    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    TrayAdd();
    ClipboardInit(g.main);
    RegisterHotkey();
    AutoStartSet(g.cfg.autostart);

    if (g.firstRun) {
        std::wstring msg = L"按 ";
        msg += FormatHotkey(g.cfg.hotkeyMods, g.cfg.hotkeyVk);
        msg += L" 粘贴上一条剪贴板内容。右键托盘图标可打开菜单。";
        TrayBalloon(L"ClipWhale 已在后台运行", msg);
    }

    TrimProcessWorkingSet();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Tab / Enter / Esc navigation inside the settings window
        if (g.settings && IsDialogMessageW(g.settings, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StoreSave(true);
    if (g.settings) DestroyWindow(g.settings);
    if (g.main) {
        // WM_DESTROY normally did this already. This catches the paths that end
        // the loop without the window being destroyed.
        if (g.suppressCapture) {
            KillTimer(g.main, TIMER_RESTORE);
            ClipboardRestoreSnapshot();
            g.suppressCapture = false;
        }
        ClipboardShutdown(g.main);
    }
    TrayRemove();
    if (g.iconBig) DestroyIcon(g.iconBig);
    if (g.iconSmall && g.iconSmall != g.iconBig) DestroyIcon(g.iconSmall);
    if (mtx) CloseHandle(mtx);
    return 0;
}
