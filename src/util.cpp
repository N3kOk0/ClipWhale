// ---------------------------------------------------------------------------
//  util.cpp : strings, paths, hotkey parsing, config, logging
//  Depends on kernel32 + a couple of user32 calls. Nothing else.
// ---------------------------------------------------------------------------
#include "common.h"
#include <cwchar>
#include <cwctype>
#include <algorithm>

// ---------------------------------------------------------------------------
int S(int px) {
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    if (dpi <= 0) dpi = 96;
    return MulDiv(px, dpi, 96);
}

void TrimProcessWorkingSet() {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// ---------------------------------------------------------------------------
std::wstring AppDataDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 2);
    cached = (n && n < MAX_PATH * 2) ? buf : L".";
    cached += L"\\ClipWhale";
    CreateDirectoryW(cached.c_str(), nullptr);
    return cached;
}

std::wstring ConfigPath()  { return AppDataDir() + L"\\config.ini"; }
std::wstring HistoryPath() { return AppDataDir() + L"\\history.dat"; }
std::wstring LogPath()     { return AppDataDir() + L"\\clipwhale.log"; }

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

// ---------------------------------------------------------------------------
void Log(const wchar_t* fmt, ...) {
    if (!g.verbose) return;
    FILE* f = _wfopen(LogPath().c_str(), L"a, ccs=UTF-8");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

// ---------------------------------------------------------------------------
std::wstring KeyName(UINT vk) {
    switch (vk) {
    case VK_SPACE:      return L"Space";
    case VK_TAB:        return L"Tab";
    case VK_RETURN:     return L"Enter";
    case VK_ESCAPE:     return L"Esc";
    case VK_BACK:       return L"Backspace";
    case VK_DELETE:     return L"Delete";
    case VK_INSERT:     return L"Insert";
    case VK_HOME:       return L"Home";
    case VK_END:        return L"End";
    case VK_PRIOR:      return L"PageUp";
    case VK_NEXT:       return L"PageDown";
    case VK_LEFT:       return L"Left";
    case VK_RIGHT:      return L"Right";
    case VK_UP:         return L"Up";
    case VK_DOWN:       return L"Down";
    case VK_OEM_3:      return L"`";
    case VK_OEM_MINUS:  return L"-";
    case VK_OEM_PLUS:   return L"=";
    case VK_OEM_4:      return L"[";
    case VK_OEM_6:      return L"]";
    case VK_OEM_5:      return L"\\";
    case VK_OEM_1:      return L";";
    case VK_OEM_7:      return L"'";
    case VK_OEM_COMMA:  return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_2:      return L"/";
    default: break;
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t buf[64] = {};
    if (GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0 && buf[0]) return buf;
    return L"VK " + std::to_wstring(vk);
}

std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    s += KeyName(vk);
    return s;
}

static void TrimTok(std::wstring& t) {
    while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
}

static bool ParseKeyName(std::wstring t, UINT& vk) {
    TrimTok(t);
    if (t.empty()) return false;

    if (t.size() >= 2 && (t[0] == L'F' || t[0] == L'f')) {
        int num = _wtoi(t.c_str() + 1);
        if (num >= 1 && num <= 24) { vk = (UINT)(VK_F1 + num - 1); return true; }
    }
    if (t.size() == 1) {
        wchar_t c = (wchar_t)towupper(t[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) { vk = (UINT)c; return true; }
    }
    struct { const wchar_t* n; UINT v; } tbl[] = {
        { L"Space", VK_SPACE }, { L"Tab", VK_TAB }, { L"Enter", VK_RETURN },
        { L"Esc", VK_ESCAPE }, { L"Delete", VK_DELETE }, { L"Insert", VK_INSERT },
        { L"Home", VK_HOME }, { L"End", VK_END }, { L"PageUp", VK_PRIOR },
        { L"PageDown", VK_NEXT }, { L"Up", VK_UP }, { L"Down", VK_DOWN },
        { L"Left", VK_LEFT }, { L"Right", VK_RIGHT },
        { L"`", VK_OEM_3 }, { L"-", VK_OEM_MINUS }, { L"=", VK_OEM_PLUS },
        { L"[", VK_OEM_4 }, { L"]", VK_OEM_6 }, { L"\\", VK_OEM_5 },
        { L";", VK_OEM_1 }, { L"'", VK_OEM_7 }, { L",", VK_OEM_COMMA },
        { L".", VK_OEM_PERIOD }, { L"/", VK_OEM_2 },
    };
    for (const auto& e : tbl)
        if (_wcsicmp(t.c_str(), e.n) == 0) { vk = e.v; return true; }
    return false;
}

bool ParseHotkey(const std::wstring& s, UINT& mods, UINT& vk) {
    UINT m = 0, key = 0;
    std::wstring rest = s;
    size_t pos = 0;
    for (;;) {
        size_t plus = rest.find(L'+', pos);
        std::wstring tok = (plus == std::wstring::npos) ? rest.substr(pos) : rest.substr(pos, plus - pos);
        TrimTok(tok);
        if (!tok.empty()) {
            if      (_wcsicmp(tok.c_str(), L"Ctrl") == 0 || _wcsicmp(tok.c_str(), L"Control") == 0) m |= MOD_CONTROL;
            else if (_wcsicmp(tok.c_str(), L"Alt") == 0)   m |= MOD_ALT;
            else if (_wcsicmp(tok.c_str(), L"Shift") == 0) m |= MOD_SHIFT;
            else if (_wcsicmp(tok.c_str(), L"Win") == 0)   m |= MOD_WIN;
            else if (!ParseKeyName(tok, key)) return false;
        }
        if (plus == std::wstring::npos) break;
        pos = plus + 1;
    }
    if (key == 0) return false;
    mods = m;
    vk = key;
    return true;
}

// ---------------------------------------------------------------------------
bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    size_t got = 0;
    while (got < out.size()) {
        DWORD chunk = (DWORD)((out.size() - got) > (1u << 20) ? (1u << 20) : (out.size() - got));
        DWORD read = 0;
        if (!ReadFile(h, out.data() + got, chunk, &read, nullptr) || read == 0) break;
        got += read;
    }
    out.resize(got);
    CloseHandle(h);
    return true;
}

bool WriteAllBytes(const std::wstring& path, const void* data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint8_t* p = (const uint8_t*)data;
    size_t put = 0;
    bool ok = true;
    while (put < len) {
        DWORD chunk = (DWORD)((len - put) > (1u << 20) ? (1u << 20) : (len - put));
        DWORD wrote = 0;
        if (!WriteFile(h, p + put, chunk, &wrote, nullptr) || wrote == 0) { ok = false; break; }
        put += wrote;
    }
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunVal = L"ClipWhale";

bool AutoStartIsOn() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG r = RegQueryValueExW(k, kRunVal, nullptr, &type, (LPBYTE)buf, &cb);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != 0;
}

void AutoStartSet(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunVal, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunVal);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
static bool ThemeFlagIsDark(const wchar_t* name, bool fallback) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return fallback;
    DWORD v = 1, cb = sizeof(v), type = 0;
    LONG r = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&v, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return fallback;
    return v == 0;                       // the flag is *UseLightTheme
}

// Drives our own windows.
bool IsSystemDarkMode() { return ThemeFlagIsDark(L"AppsUseLightTheme", false); }

// Drives the taskbar, and therefore the tray icon. These are two separate
// settings in Windows, so they can disagree.
bool IsTaskbarDark() { return ThemeFlagIsDark(L"SystemUsesLightTheme", false); }

// ---------------------------------------------------------------------------
void ConfigLoad() {
    const std::wstring p = ConfigPath();
    g.firstRun = (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES);

    Config d;
    wchar_t hk[64] = {};
    GetPrivateProfileStringW(L"Hotkey", L"Key", L"Ctrl+Alt+V", hk, 64, p.c_str());

    UINT m = 0, v = 0;
    if (ParseHotkey(hk, m, v)) { g.cfg.hotkeyMods = m; g.cfg.hotkeyVk = v; }
    else Log(L"config: cannot parse Key '%ls', using the default", hk);

    wchar_t pk[64] = {};
    GetPrivateProfileStringW(L"Hotkey", L"PasteKey", L"Ctrl+V", pk, 64, p.c_str());
    UINT pm = 0, pv = 0;
    if (ParseHotkey(pk, pm, pv)) { g.cfg.pasteMods = pm; g.cfg.pasteVk = pv; }
    else Log(L"config: cannot parse PasteKey '%ls', using Ctrl+V", pk);

    g.cfg.pasteDelayMs   = (int)GetPrivateProfileIntW(L"Paste", L"DelayMs",       d.pasteDelayMs,   p.c_str());
    g.cfg.restoreDelayMs = (int)GetPrivateProfileIntW(L"Paste", L"RestoreDelayMs", d.restoreDelayMs, p.c_str());
    g.cfg.waitReleaseMs  = (int)GetPrivateProfileIntW(L"Paste", L"WaitReleaseMs",  d.waitReleaseMs,  p.c_str());

    if (g.cfg.pasteDelayMs   < 0)    g.cfg.pasteDelayMs = 0;
    if (g.cfg.pasteDelayMs   > 2000) g.cfg.pasteDelayMs = 2000;
    if (g.cfg.restoreDelayMs < 50)   g.cfg.restoreDelayMs = 50;
    if (g.cfg.restoreDelayMs > 10000) g.cfg.restoreDelayMs = 10000;
    if (g.cfg.waitReleaseMs  < 0)    g.cfg.waitReleaseMs = 0;
    if (g.cfg.waitReleaseMs  > 3000) g.cfg.waitReleaseMs = 3000;

    g.cfg.autostart = GetPrivateProfileIntW(L"General", L"AutoStart", d.autostart ? 1 : 0, p.c_str()) != 0;
    g.cfg.trimWhitespace = GetPrivateProfileIntW(L"General", L"TrimWhitespace", 0, p.c_str()) != 0;
    g.cfg.themeMode = (int)GetPrivateProfileIntW(L"General", L"Theme", 0, p.c_str());
    if (g.cfg.themeMode < 0 || g.cfg.themeMode > 2) g.cfg.themeMode = 0;

    if (g.firstRun) ConfigSave();
}

void ConfigSave() {
    const std::wstring p = ConfigPath();
    std::wstring hk = FormatHotkey(g.cfg.hotkeyMods, g.cfg.hotkeyVk);
    WritePrivateProfileStringW(L"Hotkey", L"Key", hk.c_str(), p.c_str());
    WritePrivateProfileStringW(L"Hotkey", L"PasteKey",
                               FormatHotkey(g.cfg.pasteMods, g.cfg.pasteVk).c_str(), p.c_str());

    WritePrivateProfileStringW(L"Paste", L"DelayMs",
                               std::to_wstring(g.cfg.pasteDelayMs).c_str(), p.c_str());
    WritePrivateProfileStringW(L"Paste", L"RestoreDelayMs",
                               std::to_wstring(g.cfg.restoreDelayMs).c_str(), p.c_str());
    WritePrivateProfileStringW(L"Paste", L"WaitReleaseMs",
                               std::to_wstring(g.cfg.waitReleaseMs).c_str(), p.c_str());

    WritePrivateProfileStringW(L"General", L"AutoStart", g.cfg.autostart ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"General", L"TrimWhitespace", g.cfg.trimWhitespace ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"General", L"Theme", std::to_wstring(g.cfg.themeMode).c_str(), p.c_str());
}
