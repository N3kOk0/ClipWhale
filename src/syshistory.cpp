// ---------------------------------------------------------------------------
//  syshistory.cpp : the Windows clipboard history (Win+V), read through the
//  WinRT ABI.
//
//  Two things make this less obvious than the documentation suggests.
//
//  MinGW typedefs `boolean` as unsigned char, which is the same type as BYTE.
//  windows.foundation.h therefore declares IReference<boolean> and
//  IReference<BYTE> - twice for one type - and refuses to compile at all, on
//  its own, before anything else is even included. Renaming boolean to bool
//  first gives it a type of its own; both are one byte, so the ABI does not
//  care. The rename lives in this file on purpose.
//
//  The Clipboard class only activates in an STA. From an MTA it fails with
//  0x8000001D. And in an STA the async operation only completes while the
//  thread is pumping messages, so the wait cannot be a sleep loop.
//
//  All of it is optional. If the history is switched off, refused, or simply
//  shorter than two entries, this returns false and the caller falls back to
//  its own store.
// ---------------------------------------------------------------------------
#include "common.h"
#include <winstring.h>

#undef boolean
#define boolean bool
#include <roapi.h>
#include <windows.applicationmodel.datatransfer.h>

using namespace ABI::Windows::ApplicationModel::DataTransfer;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Foundation::Collections;

namespace {

template <class T>
struct Hold {
    T* p = nullptr;
    ~Hold() { if (p) p->Release(); }
    T** ref() { return &p; }
    T*  get() const { return p; }
    T*  operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// -1 unknown, 0 unusable on this thread, 1 ready.
int g_staState = -1;

bool EnsureSta() {
    if (g_staState >= 0) return g_staState == 1;

    HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
    // S_FALSE is "already initialised on this thread", which is fine.
    // Anything else - RPC_E_CHANGED_MODE when the thread is already MTA - means
    // the clipboard class cannot be activated here at all.
    g_staState = (hr == S_OK || hr == S_FALSE) ? 1 : 0;
    Log(L"syshistory: RoInitialize(STA) -> 0x%08lX, usable=%d",
        (unsigned long)hr, g_staState);
    return g_staState == 1;
}

// The operation only completes while this thread pumps, so this is not a sleep
// loop. WM_QUIT is put back and reported as "not done" so the real loop ends.
bool PumpUntilDone(IAsyncInfo* info, DWORD timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        AsyncStatus st = AsyncStatus::Started;
        if (FAILED(info->get_Status(&st))) return false;
        if (st != AsyncStatus::Started) return true;
        if (GetTickCount() - start >= timeoutMs) return false;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage((int)msg.wParam);
                return false;
            }
            if (g.settings && IsDialogMessageW(g.settings, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

// Text of history entry `index`, or empty.
bool TextAt(IVectorView<ClipboardHistoryItem*>* items, unsigned index,
            std::wstring& out) {
    Hold<IClipboardHistoryItem> item;
    if (FAILED(items->GetAt(index, item.ref())) || !item) return false;

    Hold<IDataPackageView> view;
    if (FAILED(item->get_Content(view.ref())) || !view) return false;

    HSTRING fmt = nullptr;
    if (FAILED(WindowsCreateString(L"Text", 4, &fmt))) return false;
    boolean hasText = 0;
    view->Contains(fmt, &hasText);
    WindowsDeleteString(fmt);
    if (!hasText) return false;

    Hold<IAsyncOperation<HSTRING>> op;
    if (FAILED(view->GetTextAsync(op.ref())) || !op) return false;

    Hold<IAsyncInfo> info;
    if (FAILED(op->QueryInterface(__uuidof(IAsyncInfo), (void**)info.ref())) || !info)
        return false;
    if (!PumpUntilDone(info.get(), 2000)) return false;

    HSTRING txt = nullptr;
    if (FAILED(op->GetResults(&txt)) || !txt) return false;

    UINT32 len = 0;
    const wchar_t* p = WindowsGetStringRawBuffer(txt, &len);
    out.assign(p, len);
    WindowsDeleteString(txt);
    return !out.empty();
}

} // namespace

// ---------------------------------------------------------------------------
bool SystemHistoryPrevious(std::wstring& out) {
    out.clear();
    if (!EnsureSta()) return false;

    const wchar_t* kClass = L"Windows.ApplicationModel.DataTransfer.Clipboard";
    HSTRING cls = nullptr;
    if (FAILED(WindowsCreateString(kClass, (UINT32)wcslen(kClass), &cls))) return false;

    Hold<IClipboardStatics2> statics;
    HRESULT hr = RoGetActivationFactory(cls, __uuidof(IClipboardStatics2),
                                        (void**)statics.ref());
    WindowsDeleteString(cls);
    if (FAILED(hr) || !statics) {
        Log(L"syshistory: activation failed 0x%08lX", (unsigned long)hr);
        return false;
    }

    Hold<IAsyncOperation<ClipboardHistoryItemsResult*>> op;
    hr = statics->GetHistoryItemsAsync(op.ref());
    if (FAILED(hr) || !op) {
        Log(L"syshistory: GetHistoryItemsAsync failed 0x%08lX", (unsigned long)hr);
        return false;
    }

    Hold<IAsyncInfo> info;
    if (FAILED(op->QueryInterface(__uuidof(IAsyncInfo), (void**)info.ref())) || !info)
        return false;
    if (!PumpUntilDone(info.get(), 2000)) {
        Log(L"syshistory: timed out waiting for the history");
        return false;
    }

    Hold<IClipboardHistoryItemsResult> res;
    if (FAILED(op->GetResults(res.ref())) || !res) return false;

    ClipboardHistoryItemsResultStatus status = ClipboardHistoryItemsResultStatus_Success;
    res->get_Status(&status);
    if (status != ClipboardHistoryItemsResultStatus_Success) {
        Log(L"syshistory: unavailable, status=%d (1=access denied, 2=history off)",
            (int)status);
        return false;
    }

    Hold<IVectorView<ClipboardHistoryItem*>> items;
    if (FAILED(res->get_Items(items.ref())) || !items) return false;

    unsigned n = 0;
    items->get_Size(&n);
    if (n < 2) {
        // Only the thing that is on the clipboard right now - there is no
        // "previous" to hand back.
        Log(L"syshistory: only %u entries", n);
        return false;
    }

    // Entry 0 is what is on the clipboard now, so entry 1 is the one before it.
    // That only holds because our own temporary writes are marked as
    // "do not keep in history" - see ClipboardSetText.
    if (!TextAt(items.get(), 1, out)) {
        Log(L"syshistory: entry 1 has no text");
        out.clear();
        return false;
    }
    return true;
}
