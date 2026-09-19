// ---------------------------------------------------------------------------
//  darkmode.cpp : Windows 10/11 dark mode plumbing.
//
//  There is no supported API for this. The three functions live in uxtheme.dll
//  with no exported names, so they can only be reached by ordinal:
//
//      132  AllowDarkModeForApp(BOOL)          Win10 1809, superseded by 135
//      133  FlushMenuThemes()                  re-theme open menus
//      135  SetPreferredAppMode(int)           Win10 1903+, the one that matters
//      136  AllowDarkModeForWindow(HWND, BOOL) opt a window in
//
//  SetPreferredAppMode is what makes user32 draw *menus* dark - including the
//  TrackPopupMenu we use for the tray. It has to be called before the menus are
//  created, and the owning window must also be opted in via 136.
//
//  All of it is optional: on a system where the ordinals do not exist every
//  call turns into a no-op and the app simply stays light.
// ---------------------------------------------------------------------------
#include "common.h"
#include <uxtheme.h>          // SetWindowTheme (the documented one)
#include <dwmapi.h>           // DwmSetWindowAttribute

namespace {

enum PreferredAppMode {
    AppModeDefault   = 0,
    AppModeAllowDark = 1,
    AppModeForceDark = 2,
    AppModeForceLight = 3
};

typedef int  (WINAPI *PFN_SetPreferredAppMode)(int);
typedef BOOL (WINAPI *PFN_AllowDarkModeForWindow)(HWND, BOOL);
typedef BOOL (WINAPI *PFN_AllowDarkModeForApp)(BOOL);
typedef void (WINAPI *PFN_FlushMenuThemes)(void);

PFN_SetPreferredAppMode    pSetPreferredAppMode = nullptr;
PFN_AllowDarkModeForWindow pAllowDarkModeForWindow = nullptr;
PFN_AllowDarkModeForApp    pAllowDarkModeForApp = nullptr;
PFN_FlushMenuThemes        pFlushMenuThemes = nullptr;

bool g_dark = false;
bool g_probed = false;

void PushAppMode() {
    // AppModeAllowDark only says "dark is permitted" - user32 still follows the
    // *system* setting, so on a light Windows the tray menu would stay light
    // even though our own windows are dark. Forcing is what actually flips the
    // menus, so use it whenever the user asked for dark explicitly.
    int mode = AppModeDefault;
    if (g.cfg.themeMode == 1)      mode = AppModeForceDark;
    else if (g.cfg.themeMode == 2) mode = AppModeForceLight;
    else                           mode = g_dark ? AppModeAllowDark : AppModeDefault;

    if (pSetPreferredAppMode) {
        pSetPreferredAppMode(mode);
    } else if (pAllowDarkModeForApp) {
        // pre-1903 fallback: no mode argument, just a boolean
        pAllowDarkModeForApp(mode == AppModeForceDark || mode == AppModeAllowDark ? TRUE : FALSE);
    }
    if (pFlushMenuThemes) pFlushMenuThemes();
    Log(L"darkmode: preferred app mode = %d", mode);
}

} // namespace

// ---------------------------------------------------------------------------
static void Probe() {
    if (g_probed) return;
    g_probed = true;

    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;

    pSetPreferredAppMode    = (PFN_SetPreferredAppMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    pAllowDarkModeForWindow = (PFN_AllowDarkModeForWindow)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    pFlushMenuThemes        = (PFN_FlushMenuThemes)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    pAllowDarkModeForApp    = (PFN_AllowDarkModeForApp)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(132));

    Log(L"darkmode: ord135=%d ord136=%d ord133=%d ord132=%d",
        pSetPreferredAppMode ? 1 : 0, pAllowDarkModeForWindow ? 1 : 0,
        pFlushMenuThemes ? 1 : 0, pAllowDarkModeForApp ? 1 : 0);
}

// ---------------------------------------------------------------------------
bool DarkModeEnabled() { return g_dark; }

// Call once, before any menu or window is created.
bool DarkModeInit() {
    Probe();

    switch (g.cfg.themeMode) {
    case 1:  g_dark = true;  break;          // forced dark
    case 2:  g_dark = false; break;          // forced light
    default: g_dark = IsSystemDarkMode();    // follow Windows
    }

    PushAppMode();
    Log(L"darkmode: enabled=%d (mode=%d)", g_dark ? 1 : 0, g.cfg.themeMode);
    return g_dark;
}

void DarkModeApply(HWND hwnd, bool dark) {
    if (!hwnd) return;
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);

    if (!dark) {
        SetWindowTheme(hwnd, nullptr, nullptr);
    } else {
        // The sub-app name has to match the control. DarkMode_Explorer is fine
        // for buttons and scrollbars, but a ComboBox keeps its light grey
        // display area with it - that one wants DarkMode_CFD, which is what the
        // shell's own dark file dialog uses.
        //
        // Check boxes are deliberately NOT special-cased here. Dropping their
        // theme would make SetTextColor work, but it also reverts them to the
        // classic glyph. Instead the label lives in a separate STATIC (see
        // settings.cpp) and the box keeps the modern themed look.
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        const wchar_t* sub = L"DarkMode_Explorer";
        if (_wcsicmp(cls, L"ComboBox") == 0 || _wcsicmp(cls, L"Edit") == 0)
            sub = L"DarkMode_CFD";
        SetWindowTheme(hwnd, sub, nullptr);
    }

    // The system title bar is a DWM affair; it does not follow the app theme on
    // its own. Attribute 20 is Win10 20H1+, 19 is 1809-1909. Harmless on child
    // windows - DWM just ignores it.
    BOOL immersive = dark ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, 20, &immersive, sizeof(immersive));
    if (FAILED(hr)) hr = DwmSetWindowAttribute(hwnd, 19, &immersive, sizeof(immersive));

    if (GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) {
        // Setting the attribute alone does not repaint the frame - without this
        // the title bar keeps whatever colour it already had.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        Log(L"darkmode: title bar %p -> 0x%08X", (void*)hwnd, (unsigned)hr);
    }
}

void DarkModeApplyTree(HWND parent, bool dark) {
    if (!parent) return;
    DarkModeApply(parent, dark);
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        DarkModeApply(c, dark);
}

// Re-read the system setting; returns true when something actually changed.
bool DarkModeRefresh() {
    if (g.cfg.themeMode != 0) return false;      // forced, ignore the system

    bool want = IsSystemDarkMode();
    if (want == g_dark) return false;

    g_dark = want;
    PushAppMode();
    if (g.main) DarkModeApply(g.main, g_dark);
    Log(L"darkmode: system changed -> enabled=%d", g_dark ? 1 : 0);
    return true;
}

// Recompute from the config (forced dark/light/auto) and re-arm everything.
// Called after the settings window saves.
bool DarkModeReevaluate() {
    Probe();

    bool want = (g.cfg.themeMode == 1) ? true
              : (g.cfg.themeMode == 2) ? false
              : IsSystemDarkMode();

    bool changed = (want != g_dark);
    g_dark = want;
    PushAppMode();
    if (g.main) DarkModeApply(g.main, g_dark);
    Log(L"darkmode: reevaluated -> enabled=%d (mode=%d)", g_dark ? 1 : 0, g.cfg.themeMode);
    return changed;
}
