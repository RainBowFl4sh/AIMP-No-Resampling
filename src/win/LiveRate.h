// 2.8: AIMP's own output rate without an AIMP restart (ASIO, WASAPI exclusive, DirectSound).
//
// AIMP plays these outputs with its own fixed rate (Preferences -> Sound Output) and offers plugins no way
// to change it. But a new rate picked on its Sound Output page is used at once when "Apply" is pressed -
// AIMP rebuilds its output while the track keeps playing. So the plugin does exactly that, invisibly:
//   1. It opens AIMP's preferences (AIMP_MSG_CMD_OPTIONS). A CBT hook notes the new window, a subclass on
//      it removes every "show" from the window positions, so the window never appears (no taskbar entry).
//      The CBT hook also keeps the window from being activated: the program in front keeps the focus. Only
//      for pressing Apply (AIMP's button needs the keyboard focus) the window may take it for one moment;
//      the window, focus and foreground window from before are put back right after. If AIMP keeps an
//      earlier (hidden) preferences window, that one is used.
//   2. The window opens on the Sound Output page: AIMP opens its preferences on the page used last
//      (Options\ActivePage), so the plugin sets the first page (Sound Output) before and puts the user's page
//      back afterwards. Apply saves every page that was shown, and some pages do more than store values (after
//      the Plugins page AIMP 6 restarts itself) - so if the window still shows another page, nothing is pressed.
//      The rate list is drawn by AIMP itself (no window text): every entry is printed into a metafile and the
//      text of its drawing is read.
//   3. It picks the first wanted rate that AIMP offers for the device, selects it, presses Apply and closes
//      the window again. AIMP's own setting (AIMPSoundOut\DeviceFreq) confirms the new rate.
// Everything runs on AIMP's main thread, driven by a timer (about 0.7 s in all). Windows only.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../AimpUtil.h"
#include "apiMessages.h"

namespace ar {
namespace win {

class LiveRate {
public:
    // got = the rate AIMP plays with afterwards (0 = unknown); ok = the wanted rate (or one of the wanted
    // rates) is set; why = text for the log when not
    struct Result {
        bool ok = false, busy = false, open = false;  // open: the user has AIMP's preferences open right now
        uint32_t got = 0;
        std::vector<uint32_t> list;  // AIMP's rate list for the device
        std::wstring why;
        int64_t ms = 0;
    };
    using Done = std::function<void(const Result&)>;

    static LiveRate& Get() {
        static LiveRate r;
        return r;
    }

    bool Busy() const { return state_ != kIdle; }
    static bool Running() { return Get().Busy(); }

    // The user has AIMP's preferences on screen (then nothing is done until they are closed)
    static bool PreferencesOpen() {
        HWND f = FindOptionsForm();
        return f && IsWindowVisible(f) && !IsIconic(f);
    }

    // Main thread. wanted: rates in order of preference. readRate: AIMP's AIMPSoundOut\DeviceFreq (main thread).
    // done is called on the main thread when finished (also when nothing was possible).
    void Start(const std::vector<uint32_t>& wanted, std::function<uint32_t()> readRate, Done done) {
        Result r;
        if (Busy()) { r.busy = true; r.why = L"a switch is running already"; if (done) done(r); return; }
        if (PreferencesOpen()) { r.open = true; r.why = L"AIMP's preferences are open"; if (done) done(r); return; }
        wanted_ = wanted;
        readRate_ = std::move(readRate);
        done_ = std::move(done);
        res_ = Result();
        t0_ = NowMs();
        ticks_ = waits_ = 0;
        dlg_ = nullptr;
        subclassed_ = false;
        hiding_ = true;
        state_ = kOpen;
        timer_ = SetTimer(nullptr, 0, 50, &LiveRate::TimerProc);
        if (!timer_) Finish(false, L"no timer");
    }

    // Finalize: stops a running switch (the window is closed, hooks are removed)
    void Cancel() {
        if (!Busy()) return;
        done_ = nullptr;
        if (dlg_ && IsWindow(dlg_)) PostMessageW(dlg_, WM_CLOSE, 0, 0);
        Cleanup();
    }

    // ---- helpers, also used by the tests
    // "44100 Hz", "44 100 Hz", "192000 Гц" -> rate; 0 = no rate
    static uint32_t ParseRate(const std::wstring& s) {
        uint32_t v = 0;
        int digits = 0;
        bool started = false;
        for (wchar_t c : s) {
            if (c >= L'0' && c <= L'9') {
                v = v * 10 + (uint32_t)(c - L'0');
                digits++;
                started = true;
                if (digits > 7) return 0;
            } else if (started && (c == L' ' || c == 0xA0 || c == 0x202F || c == L'.' || c == L',' || c == L'\'')) {
                continue;  // thousands separator
            } else if (started) {
                break;
            }
        }
        return digits >= 4 && v >= 4000 && v <= 1536000 ? v : 0;
    }

private:
    enum State { kIdle, kOpen, kFind, kApplied, kClosing };
    State state_ = kIdle;
    UINT_PTR timer_ = 0;
    HHOOK cbt_ = nullptr, cwp_ = nullptr;
    HWND dlg_ = nullptr;
    bool subclassed_ = false, hiding_ = false;
    bool allowActivate_ = false, giveBack_ = false;  // Apply: the window may take the focus / give it back
    HWND prevFg_ = nullptr, prevActive_ = nullptr, prevFocus_ = nullptr;  // before the switch
    // Every window HiddenProc was put on, with the procedure below it. Normally removed again at the end; if
    // another procedure was put on top of it meanwhile, HiddenProc stays in the chain and only passes
    // everything on (and is reused when the same window is opened again).
    std::vector<std::pair<HWND, WNDPROC>> chain_;
    int ticks_ = 0, waits_ = 0;
    int64_t t0_ = 0, appliedAt_ = 0;
    uint32_t target_ = 0;
    bool pageSet_ = false, pageHad_ = false;  // Options\ActivePage set by the plugin / the user had a value
    INT32 userPage_ = 0;
    std::vector<uint32_t> wanted_;
    std::function<uint32_t()> readRate_;
    Done done_;
    Result res_;

    // ---- windows of AIMP's preferences
    static std::wstring Cls(HWND h) {
        wchar_t c[128] = {0};
        GetClassNameW(h, c, 128);
        return c;
    }
    static BOOL CALLBACK FindFormProc(HWND h, LPARAM p) {
        if (Cls(h) == L"TAIMPOptionsForm") { *(HWND*)p = h; return FALSE; }
        return TRUE;
    }
    static HWND FindOptionsForm() {
        HWND found = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), FindFormProc, (LPARAM)&found);
        return found;
    }
    struct FindQ { const wchar_t* cls; HWND found; };
    static BOOL CALLBACK FindChildProc(HWND h, LPARAM p) {
        FindQ* q = (FindQ*)p;
        if (Cls(h) == q->cls) { q->found = h; return FALSE; }
        return TRUE;
    }
    static HWND FindChild(HWND parent, const wchar_t* cls) {
        FindQ q{cls, nullptr};
        if (parent) EnumChildWindows(parent, FindChildProc, (LPARAM)&q);
        return q.found;
    }
    // The rate list: the leftmost of the narrow lists in the top rows of the Sound Output page
    // (rate | bit depth | channels); the device list is wide (AIMP 5/6) or of its own class (AIMP 4)
    struct RateQ { HWND best; LONG left, top; };
    static BOOL CALLBACK RateProc(HWND h, LPARAM p) {
        RateQ* q = (RateQ*)p;
        RECT r;
        if (Cls(h) != L"TACLComboBox" || !GetWindowRect(h, &r) || r.right - r.left >= 200) return TRUE;
        if (r.top < q->top - 4 || (r.top <= q->top + 4 && r.left < q->left)) { q->best = h; q->left = r.left; q->top = r.top; }
        return TRUE;
    }
    static HWND RateList(HWND dlg) {
        HWND frame = FindChild(dlg, L"TAIMPOptionsFrameSoundOut");
        RateQ q{nullptr, LONG_MAX, LONG_MAX};
        if (frame) EnumChildWindows(frame, RateProc, (LPARAM)&q);
        return q.best;
    }
    // Apply: the rightmost button of the bottom row (OK | Cancel | Apply), whatever AIMP's language
    struct BtnQ { HWND best; LONG left, top; };
    static BOOL CALLBACK BtnProc(HWND h, LPARAM p) {
        BtnQ* q = (BtnQ*)p;
        RECT r;
        if (Cls(h) != L"TACLButton" || !GetWindowRect(h, &r)) return TRUE;
        if (r.top > q->top + 4 || (r.top >= q->top - 4 && r.left > q->left)) { q->best = h; q->left = r.left; q->top = r.top; }
        return TRUE;
    }
    static HWND ApplyButton(HWND dlg) {
        BtnQ q{nullptr, LONG_MIN, LONG_MIN};
        EnumChildWindows(dlg, BtnProc, (LPARAM)&q);
        return q.best;
    }
    // The page AIMP shows: all pages exist from the start, only the active one has WS_VISIBLE (the window itself
    // is hidden, so IsWindowVisible() cannot tell)
    static bool Shows(HWND frame) { return frame && (GetWindowLongPtrW(frame, GWL_STYLE) & WS_VISIBLE) != 0; }

    // AIMP opens its preferences on the page used last: the first page (Sound Output) for the plugin, the
    // user's page again afterwards
    void OpenOnSoundOutput() {
        pageSet_ = pageHad_ = false;
        auto cfg = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
        auto key = MakeString(AR_T("Options\\ActivePage"));
        if (!cfg || !key) return;
        INT32 v = 0;
        pageHad_ = Succeeded(cfg->GetValueAsInt32(key.Get(), &v));
        userPage_ = v;
        if (pageHad_ && v == 0) return;
        pageSet_ = Succeeded(cfg->SetValueAsInt32(key.Get(), 0));
    }
    void RestorePage() {
        if (!pageSet_) return;
        pageSet_ = false;
        if (!pageHad_) return;  // AIMP stores its own value when the window closes
        auto cfg = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
        auto key = MakeString(AR_T("Options\\ActivePage"));
        if (cfg && key) cfg->SetValueAsInt32(key.Get(), userPage_);
    }

    static void Key(HWND h, UINT vk) {
        SendMessageW(h, WM_KEYDOWN, vk, 1);
        SendMessageW(h, WM_KEYUP, vk, 0xC0000001);
    }

    // The text a control draws: printed into an enhanced metafile, then read from its text records
    static int CALLBACK EmfProc(HDC, HANDLETABLE*, const ENHMETARECORD* rec, int, LPARAM p) {
        std::wstring* t = (std::wstring*)p;
        if (rec->iType == EMR_EXTTEXTOUTW) {
            const EMREXTTEXTOUTW* e = (const EMREXTTEXTOUTW*)rec;
            if (!(e->emrtext.fOptions & ETO_GLYPH_INDEX) && e->emrtext.nChars < 256) {
                const wchar_t* s = (const wchar_t*)((const BYTE*)e + e->emrtext.offString);
                t->append(s, e->emrtext.nChars);
                *t += L'\n';
            }
        } else if (rec->iType == EMR_EXTTEXTOUTA) {
            const EMREXTTEXTOUTA* e = (const EMREXTTEXTOUTA*)rec;
            const char* s = (const char*)((const BYTE*)e + e->emrtext.offString);
            for (DWORD i = 0; i < e->emrtext.nChars && i < 256; i++) *t += (wchar_t)(unsigned char)s[i];
            *t += L'\n';
        }
        return 1;
    }
    static std::wstring Painted(HWND h) {
        std::wstring t;
        HDC ref = GetDC(h);
        HDC emf = CreateEnhMetaFileW(ref, nullptr, nullptr, nullptr);
        if (ref) ReleaseDC(h, ref);
        if (!emf) return t;
        SendMessageW(h, WM_PRINT, (WPARAM)emf, PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        HENHMETAFILE m = CloseEnhMetaFile(emf);
        if (m) {
            EnumEnhMetaFile(nullptr, m, EmfProc, &t, nullptr);
            DeleteEnhMetaFile(m);
        }
        return t;
    }
    static uint32_t Shown(HWND list) {
        std::wstring t = Painted(list);
        size_t a = 0;
        while (a < t.size()) {  // the first drawn text that is a rate
            size_t b = t.find(L'\n', a);
            if (b == std::wstring::npos) b = t.size();
            uint32_t v = ParseRate(t.substr(a, b - a));
            if (v) return v;
            a = b + 1;
        }
        return 0;
    }

    // ---- keeping the window invisible
    static LRESULT CALLBACK HiddenProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
        LiveRate& s = Get();
        WNDPROC old = nullptr;
        for (auto& c : s.chain_)
            if (c.first == h) old = c.second;
        if (s.hiding_ && h == s.dlg_) {
            if (m == WM_WINDOWPOSCHANGING && lp) {
                ((WINDOWPOS*)lp)->flags &= ~SWP_SHOWWINDOW;
            } else if (m == WM_SETREDRAW && wp && !IsWindowVisible(h)) {
                LRESULT r = CallWindowProcW(old, h, m, wp, lp);  // DefWindowProc sets WS_VISIBLE again: take it back
                SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) & ~WS_VISIBLE);
                return r;
            } else if (m == WM_SHOWWINDOW && wp) {
                return 0;
            }
        }
        LRESULT r = old ? CallWindowProcW(old, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
        if (m == WM_NCDESTROY)
            for (size_t i = 0; i < s.chain_.size(); i++)
                if (s.chain_[i].first == h) { s.chain_.erase(s.chain_.begin() + (long)i); break; }
        return r;
    }
    void Subclass(HWND h) {
        if (subclassed_ || !h) return;
        dlg_ = h;
        subclassed_ = true;
        for (auto& c : chain_)
            if (c.first == h) return;  // still in its chain from an earlier switch
        WNDPROC old = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
        chain_.push_back({h, old});
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)&LiveRate::HiddenProc);
    }
    void Unsubclass() {
        hiding_ = false;
        if (!subclassed_ || !dlg_) return;
        for (size_t i = 0; i < chain_.size(); i++) {
            if (chain_[i].first != dlg_) continue;
            if (!IsWindow(dlg_)) chain_.erase(chain_.begin() + (long)i);
            else if ((WNDPROC)GetWindowLongPtrW(dlg_, GWLP_WNDPROC) == &LiveRate::HiddenProc) {
                SetWindowLongPtrW(dlg_, GWLP_WNDPROC, (LONG_PTR)chain_[i].second);
                chain_.erase(chain_.begin() + (long)i);
            }
            break;  // otherwise HiddenProc stays in the chain and passes everything on (hiding_ = false)
        }
    }
    static LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp) {
        LiveRate& s = Get();
        if (code == HCBT_CREATEWND && !s.dlg_ && Cls((HWND)wp) == L"TAIMPOptionsForm") s.dlg_ = (HWND)wp;
        // AIMP activates its preferences when it opens them: not this hidden window (non-zero = prevented)
        if (code == HCBT_ACTIVATE && s.hiding_ && s.dlg_ && (HWND)wp == s.dlg_ && !s.allowActivate_) return 1;
        return CallNextHookEx(s.cbt_, code, wp, lp);
    }
    // Runs before the window procedure: at its first position change the VCL has installed its own procedure,
    // so the subclass goes on top of it
    static LRESULT CALLBACK CwpProc(int code, WPARAM wp, LPARAM lp) {
        LiveRate& s = Get();
        const CWPSTRUCT* m = (const CWPSTRUCT*)lp;
        if (code == HC_ACTION && m && s.dlg_ && m->hwnd == s.dlg_ && !s.subclassed_ && m->message == WM_WINDOWPOSCHANGING)
            s.Subclass(s.dlg_);
        return CallNextHookEx(s.cwp_, code, wp, lp);
    }

    static void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD) { Get().Step(); }

    void Step() {
        ticks_++;
        if (NowMs() - t0_ > 8000) { Finish(false, L"timeout"); return; }
        HWND d = dlg_ && IsWindow(dlg_) ? dlg_ : nullptr;
        switch (state_) {
            case kOpen: {
                state_ = kFind;
                HWND old = FindOptionsForm();  // a hidden window AIMP keeps from an earlier opening
                if (old) Subclass(old);
                cbt_ = SetWindowsHookExW(WH_CBT, &LiveRate::CbtProc, nullptr, GetCurrentThreadId());
                cwp_ = SetWindowsHookExW(WH_CALLWNDPROC, &LiveRate::CwpProc, nullptr, GetCurrentThreadId());
                auto disp = Service<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher);
                if (!disp || !cbt_ || !cwp_) { Finish(false, L"no message service / hooks"); return; }
                waits_ = 0;
                prevFg_ = GetForegroundWindow();
                prevActive_ = GetActiveWindow();
                prevFocus_ = GetFocus();
                OpenOnSoundOutput();
                disp->Send(AIMP_MSG_CMD_OPTIONS, 0, nullptr);  // AIMP creates the window here (about 0.5-1.5 s)
                if (dlg_ && !subclassed_) Subclass(dlg_);  // shown already without position change: subclass now
                return;
            }
            case kFind: {
                HWND frame = d ? FindChild(d, L"TAIMPOptionsFrameSoundOut") : nullptr;
                HWND list = frame ? RateList(d) : nullptr;
                if (!list || !Shows(frame)) {
                    ++waits_;
                    // Another page is shown (a window AIMP kept from earlier, or an AIMP that ignores the start
                    // page): Apply would save that page too, so nothing is pressed
                    if (list && waits_ > 10) {
                        res_.why = L"AIMP's preferences opened on another page";
                        Close();
                        return;
                    }
                    if (waits_ > 40) Finish(false, d ? L"Sound Output page not found" : L"AIMP's preferences did not open");
                    return;
                }
                uint32_t cur = Shown(list);
                if (!cur) { Finish(false, L"the rate list cannot be read"); return; }
                // Read the whole list: top entry, then down until it does not change any more
                for (int i = 0; i < 40; i++) Key(list, VK_UP);
                std::vector<uint32_t> all;
                for (int i = 0; i < 40; i++) {
                    uint32_t v = Shown(list);
                    if (!v || (!all.empty() && v == all.back())) break;
                    all.push_back(v);
                    Key(list, VK_DOWN);
                }
                res_.list = all;
                target_ = 0;
                int idx = -1;
                for (uint32_t w : wanted_) {
                    for (size_t i = 0; i < all.size(); i++)
                        if (all[i] == w) { target_ = w; idx = (int)i; break; }
                    if (target_) break;
                }
                if (!target_ || target_ == cur) {
                    // put the list back to the current rate (nothing changes), then close without Apply
                    for (int i = 0; i < 40; i++) Key(list, VK_UP);
                    for (size_t i = 0; i < all.size() && all[i] != cur; i++) Key(list, VK_DOWN);
                    res_.got = cur;
                    res_.ok = target_ != 0;
                    if (!target_) res_.why = L"AIMP does not offer " + Khz(wanted_.empty() ? 0 : wanted_[0]) + L" for this device";
                    Close();
                    return;
                }
                for (int i = 0; i < 40; i++) Key(list, VK_UP);
                for (int i = 0; i < idx; i++) Key(list, VK_DOWN);
                if (Shown(list) != target_) { Finish(false, L"could not select " + Khz(target_)); return; }
                HWND apply = ApplyButton(d);
                if (!apply) { Finish(false, L"Apply button not found"); return; }
                allowActivate_ = giveBack_ = true;
                SetFocus(apply);
                PostMessageW(apply, WM_KEYDOWN, VK_SPACE, 0x00390001);
                PostMessageW(apply, WM_KEYUP, VK_SPACE, 0xC0390001);
                appliedAt_ = NowMs();
                state_ = kApplied;
                return;
            }
            case kApplied: {
                GiveFocusBack();  // the keys are handled by now
                uint32_t now = readRate_ ? readRate_() : 0;
                if (now == target_ || NowMs() - appliedAt_ > 2500) {
                    res_.got = now;
                    res_.ok = now == target_;
                    if (!res_.ok) res_.why = L"AIMP still reports " + (now ? Khz(now) : std::wstring(L"?")) + L" after Apply";
                    Close();
                }
                return;
            }
            case kClosing: {
                if (d && IsWindowVisible(d)) {  // must never happen; never leave it on screen
                    Finish(res_.ok, res_.why.empty() ? L"window became visible" : res_.why);
                    return;
                }
                if (!d || ++waits_ > 6) Finish(res_.ok, res_.why);
                return;
            }
            default: return;
        }
    }

    // Window, focus and foreground window as before the switch. AIMP's own windows are restored inside AIMP;
    // the foreground goes back to another program only if AIMP took it (nothing happens if the user has
    // switched to another program meanwhile).
    static bool Ours(HWND h) {
        DWORD pid = 0;
        if (h) GetWindowThreadProcessId(h, &pid);
        return h && pid == GetCurrentProcessId();
    }
    void GiveFocusBack() {
        allowActivate_ = false;
        HWND d = dlg_ && IsWindow(dlg_) ? dlg_ : nullptr;
        if (giveBack_) {
            giveBack_ = false;
            if (d && GetActiveWindow() == d && prevActive_ && prevActive_ != d && IsWindow(prevActive_))
                SetActiveWindow(prevActive_);
            if (prevFocus_ && prevFocus_ != GetFocus() && IsWindow(prevFocus_) && !(d && IsChild(d, prevFocus_)))
                SetFocus(prevFocus_);
        }
        HWND fg = GetForegroundWindow();
        if (prevFg_ && fg != prevFg_ && (!fg || Ours(fg)) && prevFg_ != d && IsWindow(prevFg_)) SetForegroundWindow(prevFg_);
    }

    void Close() {
        state_ = kClosing;
        waits_ = 0;
        if (dlg_ && IsWindow(dlg_)) PostMessageW(dlg_, WM_CLOSE, 0, 0);
    }

    void Cleanup() {
        if (timer_) KillTimer(nullptr, timer_);
        timer_ = 0;
        if (cbt_) UnhookWindowsHookEx(cbt_);
        if (cwp_) UnhookWindowsHookEx(cwp_);
        cbt_ = cwp_ = nullptr;
        GiveFocusBack();
        Unsubclass();
        RestorePage();
        dlg_ = nullptr;
        subclassed_ = false;
        state_ = kIdle;
    }

    void Finish(bool ok, const std::wstring& why) {
        res_.ok = ok;
        if (!ok && res_.why.empty()) res_.why = why;
        if (ok) res_.why = why;
        res_.ms = NowMs() - t0_;
        if (dlg_ && IsWindow(dlg_) && state_ != kClosing) PostMessageW(dlg_, WM_CLOSE, 0, 0);
        Done d = std::move(done_);
        done_ = nullptr;
        Result r = res_;
        Cleanup();
        if (d) d(r);
    }
};

}  // namespace win
}  // namespace ar
