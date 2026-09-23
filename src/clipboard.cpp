// ---------------------------------------------------------------------------
//  clipboard.cpp : capture, write-back, snapshot/restore, and the Ctrl+V
// ---------------------------------------------------------------------------
#include "common.h"

// ---------------------------------------------------------------------------
static bool OpenRetry(HWND owner, int tries) {
    for (int i = 0; i < tries; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

static void TrimInPlace(std::wstring& s) {
    auto sp = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    size_t b = 0, e = s.size();
    while (b < e && sp(s[b])) ++b;
    while (e > b && sp(s[e - 1])) --e;
    if (b == 0 && e == s.size()) return;
    s = s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
bool ClipboardInit(HWND owner) {
    return AddClipboardFormatListener(owner) != FALSE;
}

void ClipboardShutdown(HWND owner) {
    RemoveClipboardFormatListener(owner);
}

// ---------------------------------------------------------------------------
void ClipboardCapture() {
    if (g.paused || g.suppressCapture) return;
    if (!g.main || !OpenRetry(g.main, 5)) return;

    // never re-capture what we ourselves just put there
    if (GetClipboardOwner() == g.main) { CloseClipboard(); return; }

    // honour the "do not record me" markers that password managers set
    static UINT kExclude = 0, kAllow = 0, kIgnore = 0;
    if (!kExclude) {
        kExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
        kAllow   = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
        kIgnore  = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    }
    if ((kExclude && IsClipboardFormatAvailable(kExclude)) ||
        (kIgnore  && IsClipboardFormatAvailable(kIgnore))) {
        CloseClipboard();
        Log(L"capture: skipped (excluded format)");
        return;
    }
    if (kAllow && IsClipboardFormatAvailable(kAllow)) {
        HANDLE h = GetClipboardData(kAllow);
        if (h) {
            DWORD* p = (DWORD*)GlobalLock(h);
            if (p) {
                bool allowed = (*p) != 0;
                GlobalUnlock(h);
                if (!allowed) { CloseClipboard(); Log(L"capture: skipped (opt-out)"); return; }
            }
        }
    }

    std::wstring text;

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) {
                text.assign(p, wcsnlen(p, CAP_TEXT_CHARS));
                GlobalUnlock(h);
            }
        }
    }
    if (text.empty() && IsClipboardFormatAvailable(CF_TEXT)) {
        HANDLE h = GetClipboardData(CF_TEXT);
        if (h) {
            const char* p = (const char*)GlobalLock(h);
            if (p) {
                int n = (int)strnlen(p, CAP_TEXT_CHARS);
                int wn = MultiByteToWideChar(CP_ACP, 0, p, n, nullptr, 0);
                if (wn > 0) {
                    text.resize((size_t)wn);
                    MultiByteToWideChar(CP_ACP, 0, p, n, &text[0], wn);
                }
                GlobalUnlock(h);
            }
        }
    }

    CloseClipboard();

    if (text.empty()) return;                  // images / files are not tracked
    if (g.cfg.trimWhitespace) TrimInPlace(text);
    if (text.empty() || text == g.cur) return;

    StorePush(text);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
static bool SetClipFmt(UINT fmt, const void* data, size_t bytes) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return false; }
    memcpy(p, data, bytes);
    GlobalUnlock(h);
    if (SetClipboardData(fmt, h)) return true;
    GlobalFree(h);
    return false;
}

bool ClipboardSetText(const std::wstring& text) {
    if (text.empty()) return false;
    if (!g.main || !OpenRetry(g.main, 10)) return false;

    bool ok = false;
    if (EmptyClipboard()) {
        if (SetClipFmt(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t)))
            ok = true;

        // Older applications only ever look at CF_TEXT, so publish an ANSI copy
        // as well. Costs nothing, and it is what the established tools do.
        int n = WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string a((size_t)n, '\0');
            WideCharToMultiByte(CP_ACP, 0, text.c_str(), (int)text.size(),
                                &a[0], n, nullptr, nullptr);
            a.push_back('\0');
            SetClipFmt(CF_TEXT, a.data(), a.size());
        }
    }
    CloseClipboard();
    Log(L"clipboard: set %u chars as CF_UNICODETEXT + CF_TEXT", (unsigned)text.size());
    return ok;
}

// ---------------------------------------------------------------------------
//  Snapshot / restore: the entry we are about to paste is only supposed to sit
//  on the clipboard for the duration of the keystroke, so the previous contents
//  come back afterwards and a plain Ctrl+V keeps working.
// ---------------------------------------------------------------------------
namespace {

struct SnapFmt {
    UINT     fmt = 0;
    bool     isBitmap = false;
    HBITMAP  bmp = nullptr;
    std::vector<uint8_t> bytes;
};

std::vector<SnapFmt> g_snap;
bool g_snapValid = false;

void FreeSnap() {
    for (auto& f : g_snap) if (f.bmp) DeleteObject(f.bmp);
    g_snap.clear();
    g_snapValid = false;
}

} // namespace

void ClipboardDropSnapshot() { FreeSnap(); }

bool ClipboardSnapshot() {
    FreeSnap();
    if (!g.main || !OpenRetry(g.main, 5)) return false;

    const SIZE_T kBudget = 32u * 1024u * 1024u;
    SIZE_T used = 0;
    UINT fmt = 0;

    while ((fmt = EnumClipboardFormats(fmt)) != 0) {
        if (fmt >= CF_PRIVATEFIRST && fmt <= CF_PRIVATELAST) continue;
        if (fmt >= CF_DSPTEXT && fmt <= CF_DSPENHMETAFILE) continue;
        if (fmt == CF_OWNERDISPLAY || fmt == CF_METAFILEPICT ||
            fmt == CF_ENHMETAFILE  || fmt == CF_PALETTE) continue;

        if (fmt == CF_BITMAP) {
            HBITMAP src = (HBITMAP)GetClipboardData(CF_BITMAP);
            if (src) {
                HBITMAP copy = (HBITMAP)CopyImage(src, IMAGE_BITMAP, 0, 0, 0);
                if (copy) {
                    SnapFmt s;
                    s.fmt = fmt;
                    s.isBitmap = true;
                    s.bmp = copy;
                    g_snap.push_back(std::move(s));
                }
            }
            continue;
        }

        HANDLE h = GetClipboardData(fmt);
        if (!h) continue;
        SIZE_T sz = GlobalSize(h);
        if (sz == 0 || sz > kBudget || used + sz > kBudget) continue;

        const void* p = GlobalLock(h);
        if (!p) continue;
        SnapFmt s;
        s.fmt = fmt;
        s.bytes.assign((const uint8_t*)p, (const uint8_t*)p + sz);
        GlobalUnlock(h);
        used += sz;
        g_snap.push_back(std::move(s));
    }

    CloseClipboard();
    g_snapValid = !g_snap.empty();
    Log(L"snapshot: %u formats, %u bytes", (unsigned)g_snap.size(), (unsigned)used);
    return g_snapValid;
}

bool ClipboardRestoreSnapshot() {
    if (!g.main) { FreeSnap(); return false; }

    if (!g_snapValid) {
        // Nothing was captured, which means the clipboard held nothing we could
        // duplicate - only private or unsupported formats. There is nothing to
        // put back, and clearing the clipboard instead would destroy whatever
        // those private formats were. Report success so the caller does not
        // waste retries on an impossible job.
        Log(L"restore: nothing was captured, nothing to put back");
        return true;
    }

    if (!OpenRetry(g.main, 10)) {
        // Another application is holding the clipboard. Keep the snapshot: a
        // later attempt can still put it back, and freeing it here would make
        // the loss permanent. The caller decides whether to retry.
        Log(L"restore: OpenClipboard failed, snapshot kept");
        return false;
    }

    // Everything this attempt needs is built BEFORE the clipboard is emptied.
    // A failed allocation can then never leave the user with a half-restored
    // clipboard - we just leave what was there alone.
    struct Prepared {
        UINT    fmt = 0;
        HGLOBAL h   = nullptr;                 // byte formats
        HBITMAP bmp = nullptr;                 // CF_BITMAP, a copy for this attempt
    };
    std::vector<Prepared> prep;
    prep.reserve(g_snap.size());

    auto releasePrep = [&] {
        for (auto& p : prep) {
            if (p.bmp)      DeleteObject(p.bmp);
            else if (p.h)   GlobalFree(p.h);
        }
        prep.clear();
    };

    bool ready = true;
    for (auto& f : g_snap) {
        Prepared p;
        p.fmt = f.fmt;

        if (f.isBitmap) {
            // Never hand the clipboard the snapshot's own HBITMAP. Setting it
            // transfers ownership, and a retry would then have nothing left to
            // offer. The snapshot keeps the source; each attempt gets a copy.
            if (!f.bmp) continue;
            p.bmp = (HBITMAP)CopyImage(f.bmp, IMAGE_BITMAP, 0, 0, 0);
            if (!p.bmp) { ready = false; break; }
        } else {
            p.h = GlobalAlloc(GMEM_MOVEABLE, f.bytes.size());
            if (!p.h) { ready = false; break; }
            void* dst = GlobalLock(p.h);
            if (!dst) { GlobalFree(p.h); p.h = nullptr; ready = false; break; }
            memcpy(dst, f.bytes.data(), f.bytes.size());
            GlobalUnlock(p.h);
        }
        prep.push_back(p);
    }

    if (!ready) {
        SIZE_T made = prep.size();
        releasePrep();
        CloseClipboard();
        Log(L"restore: only prepared %u of %u formats, clipboard left untouched",
            (unsigned)made, (unsigned)g_snap.size());
        return false;
    }

    EmptyClipboard();
    const SIZE_T want = prep.size();
    SIZE_T restored = 0;
    for (auto& p : prep) {
        HANDLE given = p.bmp ? (HANDLE)p.bmp : (HANDLE)p.h;
        if (SetClipboardData(p.fmt, given)) {
            p.bmp = nullptr;                   // clipboard owns it now
            p.h   = nullptr;
            ++restored;
        }
    }
    CloseClipboard();
    releasePrep();                             // whatever the clipboard refused

    if (restored != want) {
        // Partial: some formats are on the clipboard, some are not. The
        // snapshot has to survive so the next attempt can finish the job -
        // treating "something went back" as success would silently drop the
        // rest.
        Log(L"restore: partial (%u/%u formats), snapshot kept",
            (unsigned)restored, (unsigned)want);
        return false;
    }

    FreeSnap();
    Log(L"restore: ok (%u formats)", (unsigned)restored);
    return true;
}

// ---------------------------------------------------------------------------
//  Wait for the hotkey's own modifiers to be released.
//
//  This is the difference between working and not working: while Alt is still
//  logically down Windows classifies the injected V as WM_SYSKEYDOWN, and
//  nearly every application only handles Ctrl+V in WM_KEYDOWN. So the paste is
//  silently swallowed unless we wait for the user to let go first.
// ---------------------------------------------------------------------------
static const int kModWatch[] = {
    VK_LSHIFT, VK_RSHIFT, VK_SHIFT,
    VK_LCONTROL, VK_RCONTROL, VK_CONTROL,
    VK_LMENU, VK_RMENU, VK_MENU,
    VK_LWIN, VK_RWIN
};

bool WaitModifiersUp(int timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        int which = 0;
        for (int k : kModWatch) {
            if (GetAsyncKeyState(k) & 0x8000) { which = k; break; }
        }
        if (!which) {
            Log(L"modifiers released after %u ms", (unsigned)(GetTickCount() - start));
            return true;
        }
        if (GetTickCount() - start >= (DWORD)timeoutMs) {
            Log(L"modifier vk=0x%02X still down after %d ms - giving up on waiting",
                which, timeoutMs);
            return false;
        }
        Sleep(8);
    }
}

// ---------------------------------------------------------------------------
//  The keystroke that makes the target paste, plus the modifier juggling
//  around it.
//
//  Three things have to be true at once:
//
//   * The user is still holding the hotkey's modifiers at this point. If Alt is
//     logically down the V we inject is classified as WM_SYSKEYDOWN and the
//     target ignores it - nearly everything only handles Ctrl+V in
//     WM_KEYDOWN. If Shift is down the target would see Ctrl+Shift+V instead.
//     So we lift the modifiers we do not need for the paste, right around the
//     keystroke.
//
//   * It has to be instant. Waiting for the user to let go costs however long
//     they choose to hold the keys; lifting them ourselves costs nothing.
//
//   * The key state must be exactly as the user left it afterwards, so we press
//     the modifiers straight back down once the paste is out.
//
//      lift Alt/Shift/Win  ->  Ctrl down, V, V up, Ctrl up  ->  press them back
//
//  wScan is filled in as well as wVk: plenty of applications (browsers,
//  Windows Terminal, anything that thinks in physical keys) look at the scan
//  code and drop an event that has none. Extended keys need
//  KEYEVENTF_EXTENDEDKEY or they arrive as a completely different key.
// ---------------------------------------------------------------------------
static bool IsExtendedKey(UINT vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
    case VK_UP:     case VK_DOWN:   case VK_NUMLOCK: case VK_DIVIDE:
    case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN:
    case VK_APPS:   case VK_SNAPSHOT:
        return true;
    default:
        return false;
    }
}

static INPUT MakeKey(UINT vk, bool up) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk   = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0u) |
                    (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    return in;
}

void SendPasteKeys(HWND target) {
    UINT mod = VK_CONTROL;
    if      (g.cfg.pasteMods & MOD_CONTROL) mod = VK_CONTROL;
    else if (g.cfg.pasteMods & MOD_SHIFT)   mod = VK_SHIFT;
    else if (g.cfg.pasteMods & MOD_ALT)     mod = VK_MENU;

    struct Held { UINT vk; bool down; const wchar_t* name; };
    Held held[] = {
        { VK_SHIFT,   (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,   L"Shift" },
        { VK_CONTROL, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, L"Ctrl"  },
        { VK_MENU,    (GetAsyncKeyState(VK_MENU) & 0x8000) != 0,    L"Alt"   },
        { VK_LWIN,    ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0, L"Win" },
    };

    bool modHeld = false;
    for (auto& h : held) if (h.vk == mod) modHeld = h.down;

    INPUT in[16];
    UINT n = 0;

    // 1. lift every held modifier we do not need for the paste itself
    for (auto& h : held)
        if (h.down && h.vk != mod) in[n++] = MakeKey(h.vk, true);

    // 2. the paste - only press the modifier ourselves if the user is not
    //    already holding it down
    if (!modHeld) in[n++] = MakeKey(mod, false);
    in[n++] = MakeKey(g.cfg.pasteVk, false);
    in[n++] = MakeKey(g.cfg.pasteVk, true);
    if (!modHeld) in[n++] = MakeKey(mod, true);

    // 3. put them back the way the user is still holding them
    for (auto& h : held)
        if (h.down && h.vk != mod) in[n++] = MakeKey(h.vk, false);

    std::wstring heldTxt;
    for (auto& h : held) {
        if (!h.down) continue;
        if (!heldTxt.empty()) heldTxt += L"+";
        heldTxt += h.name;
    }

    UINT sent = SendInput(n, in, sizeof(INPUT));
    Log(L"paste: user holds [%ls]; %u events, %ls+%ls (vk 0x%02X scan 0x%02X) -> %u sent",
        heldTxt.empty() ? L"nothing" : heldTxt.c_str(), n,
        KeyName(mod).c_str(), KeyName(g.cfg.pasteVk).c_str(),
        (unsigned)g.cfg.pasteVk,
        (unsigned)MapVirtualKeyW(g.cfg.pasteVk, MAPVK_VK_TO_VSC),
        (unsigned)sent);

    if (sent == n) return;

    // SendInput refused (policy, sandbox, ...). Post WM_PASTE at whatever window
    // of the target thread holds the focus - standard edit controls honour it.
    HWND focus = target;
    DWORD tid = target ? GetWindowThreadProcessId(target, nullptr) : 0;
    if (tid) {
        GUITHREADINFO gti = {};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) focus = gti.hwndFocus;
    }
    Log(L"paste: falling back to WM_PASTE on %p", (void*)focus);
    if (focus) PostMessageW(focus, WM_PASTE, 0, 0);
}
