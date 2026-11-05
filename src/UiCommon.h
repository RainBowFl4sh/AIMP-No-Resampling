// Pieces of AIMP's GUI API shared by the settings page and the audio chain window.
#pragma once
#include <functional>

#include "AimpUtil.h"
#include "Lang.h"
#include "apiGUI.h"

#ifndef MB_ICONINFORMATION  // Linux: AIMP's message dialog takes the Windows flags
#define MB_OK 0
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#endif

namespace ar {

// AIMP's own message box (follows the skin); false if this AIMP version has none
inline bool ShowMessage(const tstring& text, DWORD flags = MB_OK | MB_ICONINFORMATION, HWND owner = (HWND)0) {
    auto ui = Service<IAIMPServiceUI>(IID_IAIMPServiceUI);
    Ptr<IAIMPUIMessageDialog> dlg;
    auto caption = MakeString(AR_T(AR_PLUGIN_NAME));
    auto t = MakeString(text);
    if (!ui || Failed(ui->QueryInterface(IID_IAIMPUIMessageDialog, dlg.OutV())) || !dlg || !caption || !t) return false;
    dlg->Execute(owner, caption.Get(), t.Get(), flags);
    return true;
}

// Visible text of a control: a translation key (the English text, UTF-8), a fixed text (paths) or a
// function that puts the text together. Kept with the control, so a language change can set it again.
struct Tx {
    std::function<tstring()> fn;
    Tx(const char* key) : fn([key] { return Tr(key); }) {}
    Tx(const tstring& fixed) : fn([fixed] { return fixed; }) {}
    explicit Tx(std::function<tstring()> f) : fn(std::move(f)) {}
};

class ChangeHandler : public ComObject<IAIMPUIChangeEvents> {
    std::function<void(IUnknown*)> fn_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPUIChangeEvents); }

public:
    explicit ChangeHandler(std::function<void(IUnknown*)> f) : fn_(std::move(f)) {}
    void WINAPI OnChanged(IUnknown* sender) override { if (fn_) fn_(sender); }
};

// IAIMPUIDrawEvents as AIMP calls it. On Windows AIMP passes the rectangle of OnDraw by reference (Delphi
// "const R: TRect"; by value only in AIMP for Linux, built with Lazarus). The SDK header declares it by value,
// which breaks 32-bit AIMP (wrong rectangle, unbalanced stack) - same vtable, AIMP's convention.
class AimpDrawEvents : public IUnknown {
public:
#ifdef _WIN32
    virtual void WINAPI OnDraw(IUnknown* sender, HCANVAS canvas, const RECT& r) = 0;
#else
    virtual void WINAPI OnDraw(IUnknown* sender, HCANVAS canvas, const RECT r) = 0;
#endif
};

// Drawing and mouse events of a paint box (the author's picture on the About tab)
class PaintEvents final : public AimpDrawEvents, public IAIMPUIMouseEvents {
    std::atomic<long> refs_{0};

public:
    std::function<void(HCANVAS, const RECT&)> draw;
    std::function<void(int, int, int)> mouse;  // 0 = move, 1 = left button down, 2 = up, 3 = mouse left the box

    HRESULT __unknwncall QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (EqualGUID(riid, IID_IUnknown) || EqualGUID(riid, IID_IAIMPUIDrawEvents)) *ppv = static_cast<AimpDrawEvents*>(this);
        else if (EqualGUID(riid, IID_IAIMPUIMouseEvents)) *ppv = static_cast<IAIMPUIMouseEvents*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    DWORD __unknwncall AddRef() override { return (DWORD)++refs_; }
    DWORD __unknwncall Release() override {
        long r = --refs_;
        if (r == 0) delete this;
        return (DWORD)r;
    }
#ifdef _WIN32
    void WINAPI OnDraw(IUnknown*, HCANVAS canvas, const RECT& r) override { if (draw) draw(canvas, r); }
#else
    void WINAPI OnDraw(IUnknown*, HCANVAS canvas, const RECT r) override { if (draw) draw(canvas, r); }
#endif
    void WINAPI OnMouseDoubleClick(IUnknown*, TAIMPUIMouseButton b, INT32 x, INT32 y, WORD) override {
        if (mouse && b == umbLeft) mouse(1, x, y);  // a quick second click counts as a click again
    }
    void WINAPI OnMouseDown(IUnknown*, TAIMPUIMouseButton b, INT32 x, INT32 y, WORD) override { if (mouse && b == umbLeft) mouse(1, x, y); }
    void WINAPI OnMouseLeave(IUnknown*) override { if (mouse) mouse(3, -1, -1); }
    void WINAPI OnMouseMove(IUnknown*, INT32 x, INT32 y, WORD) override { if (mouse) mouse(0, x, y); }
    void WINAPI OnMouseUp(IUnknown*, TAIMPUIMouseButton b, INT32 x, INT32 y, WORD) override { if (mouse && b == umbLeft) mouse(2, x, y); }
};

// Fixed position, anchored to the top-left corner. Without anchors AIMP's UI keeps the relative position
// while the parent grows - controls would drift or be cut off.
inline void PlaceControl(IAIMPUIControl* c, int x, int y, int w, int h) {
    if (!c) return;
    TAIMPUIControlPlacement p;
    memset(&p, 0, sizeof(p));
    p.Alignment = ualNone;
    p.Anchors.left = 1;
    p.Anchors.top = 1;
    p.Bounds.left = x;
    p.Bounds.top = y;
    p.Bounds.right = x + w;
    p.Bounds.bottom = y + h;
    c->SetPlacement(p);
}

inline void SetUiText(IAIMPPropertyList* c, int prop, const tstring& text) {
    auto s = MakeString(text);
    if (c && s) c->SetValueAsObject(prop, s.Get());
}

}  // namespace ar
