// ---------------------------------------------------------------------------
//  store.cpp : the two-entry history and its (very small) file
//
//      cur  - the text currently on the clipboard
//      prev - the one before it, which is what the hotkey pastes
//
//  On disk:
//      "CW"  u8 version  u32 len(cur)  cur  u32 len(prev)  prev
//  everything UTF-8.
// ---------------------------------------------------------------------------
#include "common.h"

static const char  kMagic[2] = { 'C', 'W' };
static const uint8_t kVersion = 1;

// ---------------------------------------------------------------------------
void StoreMarkDirty() {
    g.dirty = true;
    if (g.main) SetTimer(g.main, TIMER_SAVE, 1200, nullptr);
}

// A new copy pushes the old "current" down into "previous".
void StorePush(const std::wstring& text) {
    if (text == g.cur) return;          // nothing actually changed
    g.prev = g.cur;
    g.cur = text;
    Log(L"store: cur=%u chars, prev=%u chars", (unsigned)g.cur.size(), (unsigned)g.prev.size());
    StoreMarkDirty();
}

// ---------------------------------------------------------------------------
void StoreSave(bool force) {
    if (!force && !g.dirty) return;

    std::vector<uint8_t> buf;
    buf.reserve(64 + g.cur.size() * 3 + g.prev.size() * 3);

    auto put = [&](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        buf.insert(buf.end(), b, b + n);
    };
    auto putStr = [&](const std::wstring& s) {
        std::string u = WideToUtf8(s);
        uint32_t len = (uint32_t)u.size();
        put(&len, 4);
        if (len) put(u.data(), len);
    };

    put(kMagic, 2);
    put(&kVersion, 1);
    putStr(g.cur);
    putStr(g.prev);

    const std::wstring tmp = HistoryPath() + L".tmp";
    if (WriteAllBytes(tmp, buf.data(), buf.size())) {
        MoveFileExW(tmp.c_str(), HistoryPath().c_str(), MOVEFILE_REPLACE_EXISTING);
        g.dirty = false;
        Log(L"store: saved %u bytes", (unsigned)buf.size());
    } else {
        Log(L"store: save FAILED");
    }
}

void StoreLoad() {
    std::vector<uint8_t> buf;
    if (!ReadAllBytes(HistoryPath(), buf) || buf.size() < 11) {
        Log(L"store: no history file");
        return;
    }
    if (buf[0] != kMagic[0] || buf[1] != kMagic[1] || buf[2] != kVersion) {
        Log(L"store: bad header");
        return;
    }

    size_t at = 3;
    auto getStr = [&](std::wstring& out) -> bool {
        if (at + 4 > buf.size()) return false;
        uint32_t len = 0;
        memcpy(&len, buf.data() + at, 4);
        at += 4;
        if (len > 4 * 1024 * 1024 || at + len > buf.size()) return false;
        out = Utf8ToWide(std::string((const char*)buf.data() + at, len));
        at += len;
        return true;
    };

    if (!getStr(g.cur)) return;
    getStr(g.prev);
    g.dirty = false;
    Log(L"store: loaded cur=%u chars, prev=%u chars", (unsigned)g.cur.size(), (unsigned)g.prev.size());
}
