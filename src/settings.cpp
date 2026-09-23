// ---------------------------------------------------------------------------
//  settings.cpp : a plain Win32 options window.
//
//  Ordinary controls all the way - STATIC, EDIT, COMBOBOX, BUTTON - laid out
//  with CreateWindowExW and navigated with IsDialogMessage. No dialog template
//  in the .rc, so the Chinese strings stay in UTF-8 source instead of having to
//  survive windres' code page handling.
// ---------------------------------------------------------------------------
#include "common.h"
#include "resource.h"

namespace {

HWND  s_hotkey = nullptr;
HWND  s_pasteKey = nullptr;
HWND  s_theme = nullptr;
HWND  s_delay = nullptr;
HWND  s_restore = nullptr;
HWND  s_waitRelease = nullptr;
HWND  s_chkAuto = nullptr;
HWND  s_chkTrim = nullptr;

HFONT   s_font = nullptr;
HFONT   s_fontTitle = nullptr;
HICON   s_aboutIcon = nullptr;
HWND    s_aboutIconBox = nullptr;
WNDPROC s_hotkeyProc = nullptr;

// Everything that shows the app icon follows the *app* theme, not the taskbar
// one the tray icon uses.
//
// Two different mechanisms on purpose:
//   - the title bar / taskbar button icon is a per-window property, set with
//     WM_SETICON and re-sent whenever the theme changes;
//   - the About block is a STATIC that owns a handle we loaded ourselves, and
//     it has to be destroyed or it leaks.
void UpdateIcons() {
    if (g.settings) {
        bool dark = DarkModeEnabled();
        SendMessageW(g.settings, WM_SETICON, ICON_BIG,
                     (LPARAM)(dark ? g.iconBigDark : g.iconBig));
        SendMessageW(g.settings, WM_SETICON, ICON_SMALL,
                     (LPARAM)(dark ? g.iconSmallDark : g.iconSmall));
    }

    if (!s_aboutIconBox) return;
    HICON ic = (HICON)LoadImageW(g.inst,
                                 MAKEINTRESOURCEW(DarkModeEnabled() ? IDI_APPICON_DARK : IDI_APPICON),
                                 IMAGE_ICON, S(44), S(44), LR_DEFAULTCOLOR);
    if (!ic) return;
    SendMessageW(s_aboutIconBox, STM_SETICON, (WPARAM)ic, 0);
    if (s_aboutIcon) DestroyIcon(s_aboutIcon);
    s_aboutIcon = ic;
}

Config s_saved;               // what Cancel restores
bool   s_capturing = false;   // field has focus, hotkey is unregistered
bool   s_showCapture = false; // ...and the user has started pressing something
UINT   s_capMods = 0;
UINT   s_capVk = 0;

const int kClientW = 500;
int       kClientH = 300;
int       s_sepY = 0;         // separator is painted by us so it can follow the theme

// ---------------------------------------------------------------------------
//  colours
// ---------------------------------------------------------------------------
struct UiColors {
    COLORREF back, text, gray, field, fieldText, line;
};

UiColors Ui() {
    UiColors c;
    if (DarkModeEnabled()) {
        c.back      = RGB(32, 32, 32);
        c.text      = RGB(235, 235, 235);
        c.gray      = RGB(150, 150, 150);
        c.field     = RGB(46, 46, 46);
        c.fieldText = RGB(240, 240, 240);
        c.line      = RGB(78, 78, 78);
    } else {
        c.back      = GetSysColor(COLOR_BTNFACE);
        c.text      = GetSysColor(COLOR_BTNTEXT);
        c.gray      = GetSysColor(COLOR_GRAYTEXT);
        c.field     = GetSysColor(COLOR_WINDOW);
        c.fieldText = GetSysColor(COLOR_WINDOWTEXT);
        c.line      = GetSysColor(COLOR_3DSHADOW);
    }
    return c;
}

HBRUSH BackBrush() {
    static HBRUSH   br = nullptr;
    static COLORREF cached = CLR_INVALID;
    COLORREF want = Ui().back;
    if (!br || cached != want) {
        if (br) DeleteObject(br);
        br = CreateSolidBrush(want);
        cached = want;
    }
    return br;
}

HBRUSH FieldBrush() {
    static HBRUSH   br = nullptr;
    static COLORREF cached = CLR_INVALID;
    COLORREF want = Ui().field;
    if (!br || cached != want) {
        if (br) DeleteObject(br);
        br = CreateSolidBrush(want);
        cached = want;
    }
    return br;
}

// ---------------------------------------------------------------------------
//  paste-key presets
// ---------------------------------------------------------------------------
struct PastePreset { const wchar_t* name; UINT mods; UINT vk; };
const PastePreset kPastePresets[] = {
    { L"Ctrl+V",       MOD_CONTROL, 'V' },
    { L"Shift+Insert", MOD_SHIFT,   VK_INSERT },
};

// names shown in the 外观 combo box, and the config value each maps to
struct ThemePreset { const wchar_t* name; int mode; };
const ThemePreset kThemePresets[] = {
    { L"跟随系统", 0 },
    { L"浅色",     2 },
    { L"深色",     1 },
};

int ThemeIndexFromConfig() {
    for (int i = 0; i < (int)(sizeof(kThemePresets) / sizeof(kThemePresets[0])); ++i)
        if (kThemePresets[i].mode == g.cfg.themeMode) return i;
    return 0;
}

int PresetFromConfig() {
    for (int i = 0; i < (int)(sizeof(kPastePresets) / sizeof(kPastePresets[0])); ++i)
        if (kPastePresets[i].mods == g.cfg.pasteMods && kPastePresets[i].vk == g.cfg.pasteVk)
            return i;
    return 0;
}

// ---------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------
HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle,
        int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g.settings,
                             (HMENU)(INT_PTR)id, g.inst, nullptr);
    if (c && s_font) SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

HWND Label(const wchar_t* text, int x, int y, int w, int h) {
    return Mk(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE, 0, x, y, w, h, -1);
}

int ReadInt(HWND h, int lo, int hi, int def) {
    wchar_t b[32] = {};
    if (h) GetWindowTextW(h, b, 32);
    if (!b[0]) return def;
    int v = _wtoi(b);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void SetInt(HWND h, int v) { if (h) SetWindowTextW(h, std::to_wstring(v).c_str()); }

void UpdateHotkeyText() {
    if (!s_hotkey) return;
    // Only show the "recording" state once the user has actually started
    // pressing something - otherwise merely focusing the field blanks it and
    // you cannot see what the hotkey currently is.
    if (s_capturing && s_showCapture) {
        std::wstring s;
        if (s_capMods & MOD_CONTROL) s += L"Ctrl+";
        if (s_capMods & MOD_ALT)     s += L"Alt+";
        if (s_capMods & MOD_SHIFT)   s += L"Shift+";
        if (s_capMods & MOD_WIN)     s += L"Win+";
        s += L"…";
        SetWindowTextW(s_hotkey, s.c_str());
    } else {
        SetWindowTextW(s_hotkey, FormatHotkey(g.cfg.hotkeyMods, g.cfg.hotkeyVk).c_str());
    }
}

// ---------------------------------------------------------------------------
//  hotkey capture
//
//  The field is a normal EDIT so it looks right; it just refuses text and grabs
//  key combinations instead. While it has focus the global hotkey is
//  unregistered, otherwise pressing the current combination would trigger a
//  paste instead of being recorded.
// ---------------------------------------------------------------------------
LRESULT CALLBACK HotkeyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_GETDLGCODE: {
        const MSG* pm = (const MSG*)l;
        if (pm && (pm->message == WM_KEYDOWN || pm->message == WM_SYSKEYDOWN)) {
            UINT vk = (UINT)pm->wParam;
            if (vk == VK_TAB || vk == VK_ESCAPE) return 0;   // leave those to the dialog
        }
        return DLGC_WANTALLKEYS;
    }

    case WM_CHAR:
        return 0;                                   // never let text in

    case WM_PASTE: case WM_CUT: case WM_CLEAR:
        return 0;

    case WM_SETFOCUS:
        AppSuspendHotkey(true);
        s_capturing = true;
        s_showCapture = false;
        s_capMods = 0;
        s_capVk = 0;
        return 0;

    case WM_KILLFOCUS:
        AppSuspendHotkey(false);
        s_capturing = false;
        s_showCapture = false;
        s_capMods = 0;
        s_capVk = 0;
        UpdateHotkeyText();
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)w;

        if (vk == VK_TAB || vk == VK_ESCAPE) break;   // normal dialog behaviour

        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
            mods |= MOD_WIN;

        // a modifier on its own is not a combination yet - just show progress
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU) {
            s_capMods = mods;
            s_showCapture = true;
            UpdateHotkeyText();
            return 0;
        }

        if (mods == 0) return 0;          // refuse to steal a bare key globally

        s_capMods = mods;
        s_capVk = vk;
        s_capturing = false;
        s_showCapture = false;
        g.cfg.hotkeyMods = mods;
        g.cfg.hotkeyVk = vk;
        UpdateHotkeyText();
        return 0;
    }

    default: break;
    }
    return CallWindowProcW(s_hotkeyProc, h, m, w, l);
}

// ---------------------------------------------------------------------------
//  load / apply
// ---------------------------------------------------------------------------
void LoadControls() {
    UpdateHotkeyText();
    SendMessageW(s_pasteKey, CB_SETCURSEL, PresetFromConfig(), 0);
    SendMessageW(s_theme, CB_SETCURSEL, ThemeIndexFromConfig(), 0);
    SetInt(s_delay, g.cfg.pasteDelayMs);
    SetInt(s_restore, g.cfg.restoreDelayMs);
    SetInt(s_waitRelease, g.cfg.waitReleaseMs);
    SendMessageW(s_chkAuto, BM_SETCHECK, g.cfg.autostart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_chkTrim, BM_SETCHECK, g.cfg.trimWhitespace ? BST_CHECKED : BST_UNCHECKED, 0);
}

void ApplyAndClose() {
    int preset = (int)SendMessageW(s_pasteKey, CB_GETCURSEL, 0, 0);
    int nPresets = (int)(sizeof(kPastePresets) / sizeof(kPastePresets[0]));
    if (preset < 0 || preset >= nPresets) preset = 0;
    g.cfg.pasteMods = kPastePresets[preset].mods;
    g.cfg.pasteVk   = kPastePresets[preset].vk;

    int themeIdx = (int)SendMessageW(s_theme, CB_GETCURSEL, 0, 0);
    int nThemes = (int)(sizeof(kThemePresets) / sizeof(kThemePresets[0]));
    if (themeIdx < 0 || themeIdx >= nThemes) themeIdx = 0;
    g.cfg.themeMode = kThemePresets[themeIdx].mode;

    g.cfg.pasteDelayMs   = ReadInt(s_delay, 0, 2000, g.cfg.pasteDelayMs);
    g.cfg.restoreDelayMs = ReadInt(s_restore, 50, 10000, g.cfg.restoreDelayMs);
    g.cfg.waitReleaseMs  = ReadInt(s_waitRelease, 0, 3000, g.cfg.waitReleaseMs);

    g.cfg.autostart = SendMessageW(s_chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g.cfg.trimWhitespace = SendMessageW(s_chkTrim, BM_GETCHECK, 0, 0) == BST_CHECKED;

    ConfigSave();
    AppApplySettings();
    DestroyWindow(g.settings);
}

void CancelAndClose() {
    g.cfg = s_saved;
    AppApplySettings();
    DestroyWindow(g.settings);
}

void ResetControls() {
    Config d;
    g.cfg.hotkeyMods = d.hotkeyMods;
    g.cfg.hotkeyVk   = d.hotkeyVk;
    g.cfg.pasteMods  = d.pasteMods;
    g.cfg.pasteVk    = d.pasteVk;
    g.cfg.pasteDelayMs   = d.pasteDelayMs;
    g.cfg.restoreDelayMs = d.restoreDelayMs;
    g.cfg.waitReleaseMs  = d.waitReleaseMs;
    g.cfg.autostart  = d.autostart;
    g.cfg.trimWhitespace = d.trimWhitespace;
    g.cfg.themeMode  = d.themeMode;
    LoadControls();
}

// Picking a theme takes effect straight away, before Save - the window
// restyles itself so you can see what you picked.
void ApplyThemeLive() {
    int idx = (int)SendMessageW(s_theme, CB_GETCURSEL, 0, 0);
    int n = (int)(sizeof(kThemePresets) / sizeof(kThemePresets[0]));
    if (idx < 0 || idx >= n) return;

    g.cfg.themeMode = kThemePresets[idx].mode;
    if (DarkModeReevaluate()) {
        DarkModeApplyTree(g.settings, DarkModeEnabled());
        UpdateIcons();
        RedrawWindow(g.settings, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

// ---------------------------------------------------------------------------
//  layout
// ---------------------------------------------------------------------------
void BuildLayout() {
    const int LX = 16;          // label column
    const int LW = 136;
    const int CX = 160;         // control column
    const int CW = 170;
    const int HX = 340;         // hint column
    const int HW = kClientW - HX - 16;

    // ---------------------------------------------------------------- about
    // Everything in this block is in logical units; Mk() applies S() once.
    int y = 14;
    const int ICON = 44;

    HWND hIco = Mk(L"STATIC", L"", SS_ICON | SS_CENTERIMAGE, 0,
                   LX, y, ICON, ICON, IDC_ABOUT_ICON);
    s_aboutIcon = (HICON)LoadImageW(g.inst,
                                    MAKEINTRESOURCEW(DarkModeEnabled() ? IDI_APPICON_DARK : IDI_APPICON),
                                    IMAGE_ICON, S(ICON), S(ICON), LR_DEFAULTCOLOR);
    if (s_aboutIcon && hIco) SendMessageW(hIco, STM_SETICON, (WPARAM)s_aboutIcon, 0);
    s_aboutIconBox = hIco;

    const int tx = LX + ICON + 14;
    HWND hName = Mk(L"STATIC", APP_NAME, SS_LEFT | SS_CENTERIMAGE, 0, tx, y + 1, 300, 22, IDC_ABOUT_NAME);
    if (s_fontTitle) SendMessageW(hName, WM_SETFONT, (WPARAM)s_fontTitle, TRUE);

    std::wstring author = L"by ";
    author += APP_AUTHOR;
    author += L"   ·   v";
    author += APP_VERSION;
    Mk(L"STATIC", author.c_str(), SS_LEFT | SS_CENTERIMAGE, 0, tx, y + 23, 300, 16, IDC_ABOUT_AUTHOR);

    // A real SysLink control: the shell gives it the hand cursor, the colours
    // and keyboard activation for free, so there is nothing to draw ourselves.
    std::wstring link = L"<a href=\"";
    link += APP_URL;
    link += L"\">";
    link += APP_URL_TEXT;
    link += L"</a>";
    Mk(WC_LINK, link.c_str(), WS_TABSTOP, 0, tx - 2, y + 39, 340, 16, IDC_ABOUT_LINK);

    y += ICON + 12;

    // One short blurb, full width, in the dimmed GrayText colour (see
    // WM_CTLCOLORSTATIC). SS_LEFT without SS_CENTERIMAGE so it can wrap.
    Mk(L"STATIC",
       L"一个简单的剪贴板软件，保存最近两条复制的内容，并通过快捷键粘贴",
       SS_LEFT, 0, LX, y, kClientW - 2 * LX, 34, IDC_ABOUT_DESC);
    y += 38;

    // Painted by the window itself in WM_PAINT so it can follow the theme - an
    // SS_ETCHEDHORZ static draws with the fixed system 3D colours.
    s_sepY = y;
    y += 14;


    // ---------------------------------------------------------------- options
    Label(L"外观", LX, y + 4, LW, 18);
    s_theme = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                 CX, y, CW, 200, IDC_THEME);
    for (const auto& t : kThemePresets)
        SendMessageW(s_theme, CB_ADDSTRING, 0, (LPARAM)t.name);
    Label(L"选中后立即生效", HX, y + 4, HW, 18);
    y += 34;

    Label(L"粘贴上一条的快捷键", LX, y + 4, LW, 18);
    s_hotkey = Mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                  CX, y, CW, 24, IDC_HOTKEY);
    s_hotkeyProc = (WNDPROC)SetWindowLongPtrW(s_hotkey, GWLP_WNDPROC, (LONG_PTR)HotkeyProc);
    Label(L"点进去按组合键", HX, y + 4, HW, 18);
    y += 34;

    Label(L"注入的粘贴键", LX, y + 4, LW, 18);
    s_pasteKey = Mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                    CX, y, CW, 200, IDC_PASTEKEY);
    for (const auto& p : kPastePresets)
        SendMessageW(s_pasteKey, CB_ADDSTRING, 0, (LPARAM)p.name);
    Label(L"不生效时换这个", HX, y + 4, HW, 18);
    y += 34;

    Label(L"剪贴板稳定延迟 (ms)", LX, y + 4, LW, 18);
    s_delay = Mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE,
                 CX, y, 70, 24, IDC_DELAY);
    Label(L"0 = 不等待", HX, y + 4, HW, 18);
    y += 28;

    Label(L"还原剪贴板延迟 (ms)", LX, y + 4, LW, 18);
    s_restore = Mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE,
                   CX, y, 70, 24, IDC_RESTORE);
    Label(L"粘完后多久还原", HX, y + 4, HW, 18);
    y += 28;

    Label(L"等修饰键松开 (ms)", LX, y + 4, LW, 18);
    s_waitRelease = Mk(L"EDIT", L"", ES_NUMBER | ES_RIGHT | WS_TABSTOP, WS_EX_CLIENTEDGE,
                       CX, y, 70, 24, IDC_WAITRELEASE);
    Label(L"0 = 自动松开再按回", HX, y + 4, HW, 18);
    y += 38;

    // The label text is a separate STATIC, not the button's own caption.
    //
    // A themed check box under comctl32 v6 paints its caption in hard-coded
    // black and ignores the parent's WM_CTLCOLORSTATIC, so the text disappears
    // on a dark dialog. Dropping the control's theme makes SetTextColor work
    // again but also reverts it to the classic glyph. Keeping the box themed
    // and putting the words in a STATIC gets both: the modern glyph and a label
    // that follows the theme. SS_NOTIFY so clicking the words still toggles it.
    s_chkAuto = Mk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                   LX, y, 22, 22, IDC_CHK_AUTOSTART);
    Mk(L"STATIC", L"开机自动启动", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0,
       LX + 26, y, 160, 22, IDC_LBL_AUTOSTART);

    s_chkTrim = Mk(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                   LX, y + 26, 22, 22, IDC_CHK_TRIM);
    Mk(L"STATIC", L"去掉复制内容的首尾空白", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0,
       LX + 26, y + 26, 220, 22, IDC_LBL_TRIM);
    y += 62;

    Mk(L"BUTTON", L"恢复默认", BS_PUSHBUTTON | WS_TABSTOP, 0,
       LX, y, 90, 26, IDC_BTN_RESET);
    Mk(L"BUTTON", L"打开数据目录", BS_PUSHBUTTON | WS_TABSTOP, 0,
       LX + 98, y, 110, 26, IDC_BTN_OPENDIR);
    Mk(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
       kClientW - 16 - 90 - 8 - 90, y, 90, 26, IDC_BTN_SAVE);
    Mk(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 0,
       kClientW - 16 - 90, y, 90, 26, IDC_BTN_CANCEL);
    y += 40;

    kClientH = y;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        // CreateWindowEx has not returned yet, so g.settings is still null here;
        // without this the child controls would be created as top-level windows.
        g.settings = h;
        BuildLayout();
        LoadControls();
        DarkModeApplyTree(h, DarkModeEnabled());
        return 0;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, BackBrush());
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (s_sepY > 0) {
            RECT rc;
            GetClientRect(h, &rc);
            RECT line = { S(16), S(s_sepY), rc.right - S(16), S(s_sepY) + 1 };
            HBRUSH br = CreateSolidBrush(Ui().line);
            FillRect(dc, &line, br);
            DeleteObject(br);
        }
        EndPaint(h, &ps);
        return 0;
    }

    // Windows broadcasts this to top-level windows when the app theme flips
    case WM_SETTINGCHANGE:
        if (l && _wcsicmp((const wchar_t*)l, L"ImmersiveColorSet") == 0) {
            if (DarkModeRefresh()) {
                DarkModeApplyTree(h, DarkModeEnabled());
                UpdateIcons();
                RedrawWindow(h, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            }
        }
        return 0;

    // SetWindowTheme() sends WM_THEMECHANGED back to the window it themed, so
    // this handler must never call SetWindowTheme again - doing so recurses
    // until the stack dies. Only repaint here.
    case WM_THEMECHANGED:
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;

    case WM_COMMAND: {
        WORD id = LOWORD(w);
        WORD code = HIWORD(w);
        switch (id) {
        case IDC_THEME:
            if (code == CBN_SELCHANGE) ApplyThemeLive();
            return 0;
        // the words next to a check box are a STATIC - forward the click
        case IDC_LBL_AUTOSTART:
            if (code == STN_CLICKED) SendMessageW(s_chkAuto, BM_CLICK, 0, 0);
            return 0;
        case IDC_LBL_TRIM:
            if (code == STN_CLICKED) SendMessageW(s_chkTrim, BM_CLICK, 0, 0);
            return 0;
        case IDOK:            ApplyAndClose();   return 0;
        case IDCANCEL:        CancelAndClose();  return 0;
        case IDC_BTN_SAVE:    ApplyAndClose();   return 0;
        case IDC_BTN_CANCEL:  CancelAndClose();  return 0;
        case IDC_BTN_RESET:   ResetControls();   return 0;
        case IDC_BTN_OPENDIR: AppOpenDataDir();  return 0;
        default: break;
        }
        break;
    }

    case WM_NOTIFY: {
        // the SysLink control only tells us it was activated; the URL is ours
        NMHDR* nh = (NMHDR*)l;
        if (nh->idFrom == IDC_ABOUT_LINK &&
            (nh->code == NM_CLICK || nh->code == NM_RETURN)) {
            ShellExecuteW(nullptr, L"open", APP_URL, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        HWND ctl = (HWND)l;
        UiColors c = Ui();
        SetBkMode(dc, TRANSPARENT);
        // the blurb is supporting copy, not a label - dim it
        SetTextColor(dc, (ctl && GetDlgCtrlID(ctl) == IDC_ABOUT_DESC) ? c.gray : c.text);
        return (LRESULT)BackBrush();
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        UiColors c = Ui();
        SetTextColor(dc, c.fieldText);
        SetBkColor(dc, c.field);
        return (LRESULT)FieldBrush();
    }

    case WM_CLOSE:
        CancelAndClose();
        return 0;

    case WM_DESTROY:
        AppSuspendHotkey(false);           // make sure the hotkey comes back
        g.settings = nullptr;
        s_hotkey = s_pasteKey = s_delay = s_restore = s_waitRelease = nullptr;
        s_chkAuto = s_chkTrim = nullptr;
        s_aboutIconBox = nullptr;
        if (s_aboutIcon) { DestroyIcon(s_aboutIcon); s_aboutIcon = nullptr; }
        // Delete each font exactly once. Clearing s_font first would turn the
        // second test into "s_fontTitle != nullptr", which is true whenever both
        // names point at the same handle - the fallback when CreateFontIndirect
        // failed - and that is a double delete.
        if (s_fontTitle && s_fontTitle != s_font) DeleteObject(s_fontTitle);
        if (s_font) DeleteObject(s_font);
        s_font = s_fontTitle = nullptr;
        // Do not trim from in here. WM_DESTROY is still running, so every page
        // this code touches is faulted straight back in and the trim is wasted.
        // A moment later, once the destruction has settled, it actually works.
        if (g.main) SetTimer(g.main, TIMER_TRIM, 600, nullptr);
        return 0;

    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void CreateUiFont() {
    if (s_font) return;
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        // 微软雅黑 UI keeps the Chinese labels from looking cramped
        lstrcpynW(ncm.lfMessageFont.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
        s_font = CreateFontIndirectW(&ncm.lfMessageFont);

        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfWeight = FW_SEMIBOLD;
        lf.lfHeight = (LONG)(ncm.lfMessageFont.lfHeight * 1.4);
        s_fontTitle = CreateFontIndirectW(&lf);
    }
    if (!s_font) s_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!s_fontTitle) s_fontTitle = s_font;
}

} // namespace

// ---------------------------------------------------------------------------
bool SettingsRegister(HINSTANCE inst) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // Without these the title bar and the taskbar button fall back to the
    // generic Windows application icon - the class is where Windows looks first.
    wc.hIcon         = DarkModeEnabled() ? g.iconBigDark : g.iconBig;
    wc.hIconSm       = DarkModeEnabled() ? g.iconSmallDark : g.iconSmall;
    wc.hbrBackground = nullptr;      // painted in WM_ERASEBKGND, follows the theme
    wc.lpszClassName = WND_CLASS_SET;
    return RegisterClassExW(&wc) != 0;
}

void SettingsClose() {
    if (g.settings) SendMessageW(g.settings, WM_CLOSE, 0, 0);
}

void SettingsShow() {
    if (g.settings) {
        ShowWindow(g.settings, SW_SHOW);
        SetForegroundWindow(g.settings);
        return;
    }

    s_saved = g.cfg;                       // Cancel restores this
    CreateUiFont();

    // No WS_EX_TOOLWINDOW: a tool window gets the *small* caption, which makes
    // the system draw a shrunken close button floating inside a normal-height
    // bar. Giving it g.main as its owner instead keeps it out of the taskbar
    // and out of Alt+Tab while still using the standard caption.
    const DWORD kExStyle = WS_EX_CONTROLPARENT;
    const DWORD kStyle   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;

    RECT rc = {0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectEx(&rc, kStyle, FALSE, kExStyle);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;

    g.settings = CreateWindowExW(kExStyle, WND_CLASS_SET, L"ClipWhale 设置",
                                 kStyle, x, y, ww, wh, g.main, nullptr, g.inst, nullptr);
    if (!g.settings) { g.cfg = s_saved; return; }

    // AdjustWindowRectEx computes the frame in system-DPI units, so on a scaled
    // monitor the client area comes out the wrong size. Measure the real client
    // rect and correct the window instead of guessing.
    {
        RECT cr;
        GetClientRect(g.settings, &cr);
        int dw = S(kClientW) - (int)(cr.right - cr.left);
        int dh = S(kClientH) - (int)(cr.bottom - cr.top);
        if (dw || dh) {
            RECT wr;
            GetWindowRect(g.settings, &wr);
            SetWindowPos(g.settings, nullptr, 0, 0,
                         (wr.right - wr.left) + dw, (wr.bottom - wr.top) + dh,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    ShowWindow(g.settings, SW_SHOW);
    // Re-apply to the whole tree now the window is really on screen. Doing it
    // only in WM_CREATE is too early for the controls: SetWindowTheme returns
    // S_OK but the check boxes still paint with the classic theme.
    DarkModeApplyTree(g.settings, DarkModeEnabled());
    // The window class only carries the icon that was current when it was
    // registered, so a window created after a theme change has to be told again.
    UpdateIcons();
    SetForegroundWindow(g.settings);
    SetFocus(s_hotkey);
}
