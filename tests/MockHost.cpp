// Minimal AIMP stand-in for testing the plugin without AIMP.
// Loads PreventResampling.so/.dll, calls Initialize, simulates track starts with different sample
// rates, runs "main thread" tasks like AIMP does and finalizes the plugin again.
// Linux: expects a fake "pw-metadata" in PATH that writes its arguments to $AR_PW_LOG.
// Windows (also under Wine): checks flow, threading and configuration without real hardware.
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "apiActions.h"
#include "apiCore.h"
#include "apiFileManager.h"
#include "apiMenu.h"
#include "apiMessages.h"
#include "apiOptions.h"
#include "apiPlayer.h"
#include "apiPlaylists.h"
#include "apiPlugin.h"
#include "apiInternet.h"
#include "apiMUI.h"
#include "apiThreading.h"
#include "../src/Chain.h"  // tested directly: the audio chain model and the rules of the General tab
#include "../src/Rules.h"
#include "../src/Sha256.h"
#include "mock_ui.h"

static int g_failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { printf("FAILED: %s (line %d)\n", #c, __LINE__); g_failures++; } \
    } while (0)

typedef std::basic_string<TChar> tstr;
static tstr T(const std::string& a) { return tstr(a.begin(), a.end()); }  // ASCII only
static std::string N(const tstr& w) {
    std::string r;
    for (TChar c : w) r += (c > 0 && c < 128) ? (char)c : '?';
    return r;
}
// Characters of a text (Linux: UTF-8, continuation bytes do not count)
static size_t Len(const tstr& w) {
    size_t n = 0;
    for (TChar c : w)
        if (sizeof(TChar) > 1 || (((unsigned char)c) & 0xC0) != 0x80) n++;
    return n;
}
// UTF-8 of a text (for comparisons with non-ASCII texts)
static std::string U8(const tstr& w) {
    std::string r;
    for (size_t i = 0; i < w.size(); i++) {
        unsigned long c = (unsigned long)(sizeof(TChar) > 1 ? (unsigned long)w[i] : (unsigned char)w[i]);
        if (sizeof(TChar) == 1 || c < 0x80) { r += (char)c; continue; }
        if (c < 0x800) { r += (char)(0xC0 | (c >> 6)); r += (char)(0x80 | (c & 0x3F)); continue; }
        r += (char)(0xE0 | (c >> 12)); r += (char)(0x80 | ((c >> 6) & 0x3F)); r += (char)(0x80 | (c & 0x3F));
    }
    return r;
}

template <typename Base>
struct Obj : Base {
    std::atomic<long> refs{1};
    std::vector<const GUID*> iids;
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override {
        if (EqualGUID(riid, IID_IUnknown)) { *p = this; this->AddRef(); return S_OK; }
        for (auto* g : iids)
            if (EqualGUID(riid, *g)) { *p = this; this->AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    DWORD AddRef() override { return ++refs; }
    DWORD Release() override { long r = --refs; if (!r) delete this; return r; }
};

struct Str : Obj<IAIMPString> {
    tstr s;
    Str() { iids = {&IID_IAIMPString}; }
    HRESULT Get(INT32, TChar*) override { return E_NOTIMPL; }
    PChar GetData() override { return &s[0]; }
    INT32 GetLength() override { return (INT32)s.size(); }
    INT32 GetHashCode() override { return 0; }
    HRESULT Set(INT32, TChar) override { return E_NOTIMPL; }
    HRESULT SetData(PChar c, INT32 n) override { s.assign(c, n); return S_OK; }
    HRESULT Add(IAIMPString*) override { return E_NOTIMPL; }
    HRESULT Add2(PChar, INT32) override { return E_NOTIMPL; }
    HRESULT ChangeCase(INT32) override { return E_NOTIMPL; }
    HRESULT Clone(IAIMPString**) override { return E_NOTIMPL; }
    HRESULT Compare(IAIMPString*, INT32*, BOOL) override { return E_NOTIMPL; }
    HRESULT Compare2(PChar, INT32, INT32*, BOOL) override { return E_NOTIMPL; }
    HRESULT Delete(INT32, INT32) override { return E_NOTIMPL; }
    HRESULT Find(IAIMPString*, INT32*, INT32, INT32) override { return E_NOTIMPL; }
    HRESULT Find2(TChar*, INT32, INT32*, INT32, INT32) override { return E_NOTIMPL; }
    HRESULT Insert(INT32, IAIMPString*) override { return E_NOTIMPL; }
    HRESULT Insert2(INT32, TChar*, INT32) override { return E_NOTIMPL; }
    HRESULT Replace(IAIMPString*, IAIMPString*, INT32) override { return E_NOTIMPL; }
    HRESULT Replace2(TChar*, INT32, TChar*, INT32, INT32) override { return E_NOTIMPL; }
    HRESULT SubString(INT32, INT32, IAIMPString**) override { return E_NOTIMPL; }
};
static Str* MkStr(const std::string& v) { auto* s = new Str(); s->s = T(v); return s; }

struct FileInfo : Obj<IAIMPFileInfo> {
    int rate = 44100, bits = 24;
    std::string codec = "FLAC";
    std::string file;  // empty: a local file named after the rate
    FileInfo() { iids = {&IID_IAIMPFileInfo, &IID_IAIMPPropertyList}; }
    void BeginUpdate() override {}
    void EndUpdate() override {}
    HRESULT Reset() override { return S_OK; }
    HRESULT GetValueAsFloat(INT32, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(INT32 id, INT32* v) override {
        if (id == AIMP_FILEINFO_PROPID_SAMPLERATE) { *v = rate; return S_OK; }
        if (id == AIMP_FILEINFO_PROPID_BITDEPTH) { *v = bits; return S_OK; }
        if (id == AIMP_FILEINFO_PROPID_CHANNELS) { *v = 2; return S_OK; }
        return E_FAIL;
    }
    HRESULT GetValueAsInt64(INT32, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsObject(INT32 id, CONSTIID, void** o) override {
        if (id == AIMP_FILEINFO_PROPID_CODEC) { *o = MkStr(codec); return S_OK; }
        if (id == AIMP_FILEINFO_PROPID_FILENAME) { *o = MkStr(file.empty() ? "/musik/track" + std::to_string(rate) + ".flac" : file); return S_OK; }
        return E_FAIL;
    }
    HRESULT SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt32(INT32, INT32) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
    HRESULT SetValueAsObject(INT32, IUnknown*) override { return E_NOTIMPL; }
    HRESULT Assign(IAIMPFileInfo*) override { return E_NOTIMPL; }
    HRESULT Clone(IAIMPFileInfo**) override { return E_NOTIMPL; }
};

// A playlist (only its name is read) and an item of it
template <typename Base>
struct PropsOf : Obj<Base> {  // property list that stores what is set
    std::map<int, long long> ints;
    std::map<int, tstr> strs;
    std::map<int, IUnknown*> objs;
    ~PropsOf() { for (auto& kv : objs) kv.second->Release(); }
    void BeginUpdate() override {}
    void EndUpdate() override {}
    HRESULT Reset() override { return S_OK; }
    HRESULT GetValueAsFloat(INT32, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(INT32 id, INT32* v) override {
        auto it = ints.find(id);
        if (it == ints.end()) return E_FAIL;
        *v = (INT32)it->second;
        return S_OK;
    }
    HRESULT GetValueAsInt64(INT32, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsObject(INT32 id, CONSTIID iid, void** o) override {
        auto st = strs.find(id);
        if (st != strs.end() && EqualGUID(iid, IID_IAIMPString)) {
            auto* x = new Str();
            x->s = st->second;
            *o = x;
            return S_OK;
        }
        auto it = objs.find(id);
        if (it == objs.end()) return E_FAIL;
        return it->second->QueryInterface(iid, o);
    }
    HRESULT SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt32(INT32 id, INT32 v) override { ints[id] = v; return S_OK; }
    HRESULT SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
    HRESULT SetValueAsObject(INT32 id, IUnknown* v) override {
        IAIMPString* str = nullptr;
        if (v && Succeeded(v->QueryInterface(IID_IAIMPString, (void**)&str)) && str) {
            strs[id] = tstr(str->GetData(), str->GetLength());
            str->Release();
            return S_OK;
        }
        if (v) v->AddRef();
        if (objs.count(id)) objs[id]->Release();
        if (v) objs[id] = v;
        else objs.erase(id);
        return S_OK;
    }
    std::string S(int id) { return strs.count(id) ? N(strs[id]) : std::string(); }
};
struct PlaylistProps : PropsOf<IAIMPPropertyList> {
    PlaylistProps() { iids = {&IID_IAIMPPropertyList}; }
};
struct Playlist : Obj<IAIMPPlaylist> {
    PlaylistProps* props = new PlaylistProps();
    explicit Playlist(const std::string& name) { iids = {&IID_IAIMPPlaylist}; props->strs[AIMP_PLAYLIST_PROPID_NAME] = T(name); }
    ~Playlist() { props->Release(); }
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override {
        if (EqualGUID(riid, IID_IAIMPPropertyList)) { props->AddRef(); *p = props; return S_OK; }
        return Obj<IAIMPPlaylist>::QueryInterface(riid, p);
    }
    HRESULT Add(IUnknown*, DWORD, INT32) override { return E_NOTIMPL; }
    HRESULT AddList(IAIMPObjectList*, DWORD, INT32) override { return E_NOTIMPL; }
    HRESULT Delete(IAIMPPlaylistItem*) override { return E_NOTIMPL; }
    HRESULT Delete2(INT32) override { return E_NOTIMPL; }
    HRESULT Delete3(DWORD, TAIMPPlaylistDeleteProc, void*) override { return E_NOTIMPL; }
    HRESULT DeleteAll() override { return E_NOTIMPL; }
    HRESULT Sort(INT32) override { return E_NOTIMPL; }
    HRESULT Sort2(IAIMPString*) override { return E_NOTIMPL; }
    HRESULT Sort3(TAIMPPlaylistCompareProc*, void*) override { return E_NOTIMPL; }
    HRESULT BeginUpdate() override { return S_OK; }
    HRESULT EndUpdate() override { return S_OK; }
    HRESULT Close(DWORD) override { return E_NOTIMPL; }
    HRESULT GetFiles(DWORD, IAIMPObjectList**) override { return E_NOTIMPL; }
    HRESULT MergeGroup(IAIMPPlaylistGroup*) override { return E_NOTIMPL; }
    HRESULT ReloadFromPreimage() override { return E_NOTIMPL; }
    HRESULT ReloadInfo(DWORD) override { return E_NOTIMPL; }
    HRESULT GetItem(INT32, CONSTIID, void**) override { return E_NOTIMPL; }
    INT32 GetItemCount() override { return 0; }
    HRESULT GetGroup(INT32, CONSTIID, void**) override { return E_NOTIMPL; }
    INT32 GetGroupCount() override { return 0; }
    HRESULT ListenerAdd(IAIMPPlaylistListener*) override { return E_NOTIMPL; }
    HRESULT ListenerRemove(IAIMPPlaylistListener*) override { return E_NOTIMPL; }
};
struct PlaylistItem : PropsOf<IAIMPPlaylistItem> {
    explicit PlaylistItem(const std::string& playlist) {
        iids = {&IID_IAIMPPlaylistItem, &IID_IAIMPPropertyList};
        auto* pl = new Playlist(playlist);
        objs[AIMP_PLAYLISTITEM_PROPID_PLAYLIST] = static_cast<IAIMPPlaylist*>(pl);
    }
    HRESULT ReloadInfo() override { return S_OK; }
};

// Player: IAIMPServicePlayer + IAIMPPropertyList (for AIMP_PLAYER_PROPID_OUTPUT)
struct PlayerProps;
struct Player : Obj<IAIMPServicePlayer> {
    FileInfo* info = new FileInfo();
    PlayerProps* props = nullptr;
    int stops = 0, plays = 0, reinits = 0;
    int state = AIMP_PLAYER_STATE_PLAYING;
    float volume = 1.0f;
    std::string playlist;  // name of the playlist the track plays from ("" = none)
    bool aimp4 = false;  // AIMP 4: no AIMP_PLAYER_PROPID_OUTPUT
#ifdef _WIN32
    std::string output = "WASAPI: Lautsprecher (Mock Audio)";
#else
    std::string output = "ALSA: default";
#endif
    Player();
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override;
    HRESULT Play(IAIMPPlaybackQueueItem*) override { return E_NOTIMPL; }
    HRESULT Play2(IAIMPPlaylistItem*) override { plays++; return S_OK; }
    HRESULT Play3(IAIMPPlaylist*) override { return E_NOTIMPL; }
    HRESULT Play4(IAIMPString*, DWORD) override { plays++; return S_OK; }
    HRESULT GoToNext() override { return E_NOTIMPL; }
    HRESULT GoToPrev() override { return E_NOTIMPL; }
    HRESULT GetDuration(double*) override { return E_NOTIMPL; }
    HRESULT GetPosition(double*) override { return E_NOTIMPL; }
    double seekPos = -1;  // like AIMP 4 (no IAIMPServicePlayer2): position is set after Play
    int seekFailures = 0;  // AIMP 4 opens the track asynchronously: the first seeks can fail
    HRESULT SetPosition(const double v) override {
        if (seekFailures > 0) { seekFailures--; return E_FAIL; }
        seekPos = v;
        return S_OK;
    }
    HRESULT GetMute(BOOL*) override { return E_NOTIMPL; }
    HRESULT SetMute(const BOOL) override { return E_NOTIMPL; }
    HRESULT GetVolume(float* v) override { *v = volume; return S_OK; }
    HRESULT SetVolume(const float v) override { volume = v; return S_OK; }
    HRESULT GetInfo(IAIMPFileInfo** fi) override { info->AddRef(); *fi = info; return S_OK; }
    HRESULT GetPlaylistItem(IAIMPPlaylistItem** item) override {
        if (playlist.empty()) return E_FAIL;
        *item = new PlaylistItem(playlist);
        return S_OK;
    }
    int GetState() override { return state; }
    HRESULT Pause() override { return E_NOTIMPL; }
    HRESULT Resume() override { return E_NOTIMPL; }
    HRESULT Stop() override { stops++; return S_OK; }
    HRESULT StopAfterTrack() override { return E_NOTIMPL; }
};
struct PlayerProps : Obj<IAIMPPropertyList> {
    Player* owner;
    explicit PlayerProps(Player* p) : owner(p) { iids = {&IID_IAIMPPropertyList}; }
    void BeginUpdate() override {}
    void EndUpdate() override {}
    HRESULT Reset() override { return S_OK; }
    HRESULT GetValueAsFloat(INT32, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(INT32, INT32*) override { return E_FAIL; }
    HRESULT GetValueAsInt64(INT32, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsObject(INT32 id, CONSTIID iid, void** o) override {
        if (owner->aimp4) return E_FAIL;
        if (id == AIMP_PLAYER_PROPID_OUTPUT && EqualGUID(iid, IID_IAIMPString)) { *o = MkStr(owner->output); return S_OK; }
        return E_FAIL;
    }
    HRESULT SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt32(INT32, INT32) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
    HRESULT SetValueAsObject(INT32 id, IUnknown* v) override {
        if (id != AIMP_PLAYER_PROPID_OUTPUT || !v) return E_NOTIMPL;
        IAIMPString* str = nullptr;
        if (Failed(v->QueryInterface(IID_IAIMPString, (void**)&str)) || !str) return E_INVALIDARG;
        owner->output = N(tstr(str->GetData(), str->GetLength()));
        owner->reinits++;
        str->Release();
        return S_OK;
    }
};
Player::Player() { iids = {&IID_IAIMPServicePlayer}; props = new PlayerProps(this); }
HRESULT Player::QueryInterface(REFIID riid, LPVOID* p) {
    if (EqualGUID(riid, IID_IAIMPPropertyList)) { props->AddRef(); *p = props; return S_OK; }
    return Obj<IAIMPServicePlayer>::QueryInterface(riid, p);
}

// "Main thread": tasks from other threads are executed in pump()
struct MainQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    std::thread::id mainId = std::this_thread::get_id();
    void Pump() {
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard<std::mutex> g(m);
                if (q.empty()) return;
                f = std::move(q.front());
                q.pop_front();
            }
            f();
        }
    }
} g_main;

struct Threads : Obj<IAIMPServiceThreads> {
    Threads() { iids = {&IID_IAIMPServiceThreads}; }
    HRESULT ExecuteInMainThread(IAIMPTask* t, DWORD flags) override {
        if (std::this_thread::get_id() == g_main.mainId) { t->Execute(nullptr); return S_OK; }
        t->AddRef();
        auto done = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> g(g_main.m);
            g_main.q.push_back([t, done] { t->Execute(nullptr); t->Release(); *done = true; });
        }
        if (flags & AIMP_SERVICE_THREADS_FLAGS_WAITFOR)
            while (!*done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return S_OK;
    }
    HRESULT ExecuteInThread(IAIMPTask*, TTaskHandle*) override { return E_NOTIMPL; }
    HRESULT Cancel(TTaskHandle, DWORD) override { return E_NOTIMPL; }
    HRESULT WaitFor(TTaskHandle) override { return E_NOTIMPL; }
};

#ifdef _WIN32
static void OpenMockOptions();  // AIMP_MSG_CMD_OPTIONS (see MockOptions below)
#endif

struct Dispatcher : Obj<IAIMPServiceMessageDispatcher> {
    IAIMPMessageHook* hook = nullptr;
    int notifications = 0;
    tstr notification;  // the last text shown with AIMP_MSG_CMD_SHOW_NOTIFICATION
    INT32 equalizer = 0;
    Dispatcher() { iids = {&IID_IAIMPServiceMessageDispatcher}; }
    HRESULT Send(DWORD msg, INT32 p1, void* p2) override {
        if (msg == (DWORD)AIMP_MSG_CMD_SHOW_NOTIFICATION) {
            notifications++;
            notification = p2 ? tstr((const TChar*)p2) : tstr();
            return S_OK;
        }
        if (msg == (DWORD)AIMP_MSG_PROPERTY_HWND && p1 == AIMP_MPH_MAINFORM && p2) {
            *(HWND*)p2 = (HWND)0x4712;
            return S_OK;
        }
#ifdef _WIN32
        if (msg == (DWORD)AIMP_MSG_CMD_OPTIONS) {
            OpenMockOptions();
            return S_OK;
        }
#endif
        if (msg == (DWORD)AIMP_MSG_PROPERTY_EQUALIZER && p2) {
            if (p1 == AIMP_MSG_PROPVALUE_GET) *(INT32*)p2 = equalizer;
            else equalizer = *(INT32*)p2;
            return S_OK;
        }
        return E_NOTIMPL;
    }
    DWORD Register(PChar) override { return 0; }
    HRESULT Hook(IAIMPMessageHook* h) override { h->AddRef(); hook = h; return S_OK; }
    HRESULT Unhook(IAIMPMessageHook* h) override { if (h == hook) { hook->Release(); hook = nullptr; } return S_OK; }
};

struct ConfigSvc : Obj<IAIMPServiceConfig> {
    std::vector<std::string> deleted;
    std::map<std::string, std::string> v;
    ConfigSvc() { iids = {&IID_IAIMPServiceConfig}; }
    static std::string K(IAIMPString* s) { return N(tstr(s->GetData(), s->GetLength())); }
    void CheckMain() { CHECK(std::this_thread::get_id() == g_main.mainId); }  // AIMP allows the main thread only
    HRESULT Delete(IAIMPString* k) override { CheckMain(); deleted.push_back(K(k)); return S_OK; }
    HRESULT GetValueAsFloat(IAIMPString*, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(IAIMPString* k, INT32* r) override {
        CheckMain();
        auto it = v.find(K(k));
        if (it == v.end() || it->second.empty()) return E_FAIL;
        *r = std::atoi(it->second.c_str());
        return S_OK;
    }
    HRESULT GetValueAsInt64(IAIMPString*, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsStream(IAIMPString*, IAIMPStream**) override { return E_FAIL; }
    HRESULT GetValueAsString(IAIMPString* k, IAIMPString** r) override {
        CheckMain();
        auto it = v.find(K(k));
        if (it == v.end()) return E_FAIL;
        *r = MkStr(it->second);
        return S_OK;
    }
    HRESULT SetValueAsFloat(IAIMPString*, const DOUBLE) override { return S_OK; }
    HRESULT SetValueAsInt32(IAIMPString* k, INT32 x) override { CheckMain(); v[K(k)] = std::to_string(x); return S_OK; }
    HRESULT SetValueAsInt64(IAIMPString*, const INT64) override { return S_OK; }
    HRESULT SetValueAsStream(IAIMPString*, IAIMPStream*) override { return S_OK; }
    HRESULT SetValueAsString(IAIMPString* k, IAIMPString* x) override { CheckMain(); v[K(k)] = K(x); return S_OK; }
    HRESULT FlushCache() override { return S_OK; }
};

#ifdef _WIN32
// 2.8: AIMP's preferences for the switch without restart (src/win/LiveRate.h). The same window classes as AIMP
// (TAIMPOptionsForm, TAIMPOptionsMenu, TAIMPOptionsFrameSoundOut, TAIMPOptionsFramePlugins, TACLComboBox,
// TACLButton); the rate list draws its entries itself and has no window text, like AIMP's; Apply (Space on the
// rightmost bottom button) writes the selected rate to AIMPSoundOut\DeviceFreq. Like AIMP, the window opens on the
// page used last (Options\ActivePage, stored when it closes: 0 = Sound Output, 17 = Plugins), and Apply after the
// Plugins page was shown makes AIMP 6 restart itself (counted in restarts).
struct MockOptions {
    ConfigSvc* config = nullptr;
    std::vector<int> rates{8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000};
    int sel = 0;
    int opened = 0, applied = 0, visible = 0;  // visible: times the window came on screen
    int restarts = 0;            // Apply after the Plugins page was shown
    int activated = 0;           // times the window was activated (only allowed for the moment of Apply)
    bool keep = false;           // WM_CLOSE only hides the window (an AIMP that keeps its preferences)
    bool pluginsShown = false;   // the Plugins page was shown since the window was opened
    HWND form = nullptr, menu = nullptr, page = nullptr, plugins = nullptr, list = nullptr;
    int Current() {
        auto it = config->v.find("AIMPSoundOut\\DeviceFreq");
        return it == config->v.end() ? 0 : std::atoi(it->second.c_str());
    }
    void Show(bool sound) {  // switch the page (as a click in the menu)
        ShowWindow(page, sound ? SW_SHOW : SW_HIDE);
        ShowWindow(plugins, sound ? SW_HIDE : SW_SHOW);
        if (!sound) pluginsShown = true;
    }
    void StorePage() { config->v["Options\\ActivePage"] = (GetWindowLongPtrW(plugins, GWL_STYLE) & WS_VISIBLE) ? "17" : "0"; }
} g_opts;

static LRESULT CALLBACK MockOptionsProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);
    std::wstring c = cls;
    if (c == L"TAIMPOptionsForm") {
        if (m == WM_CLOSE) {
            g_opts.StorePage();
            if (g_opts.keep) ShowWindow(h, SW_HIDE);
            else DestroyWindow(h);
            return 0;
        }
        if (m == WM_WINDOWPOSCHANGED && IsWindowVisible(h)) g_opts.visible++;
        if (m == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) g_opts.activated++;
        if (m == WM_DESTROY) g_opts.form = g_opts.menu = g_opts.page = g_opts.plugins = g_opts.list = nullptr;
    } else if (c == L"TACLComboBox" && h == g_opts.list) {
        if (m == WM_KEYDOWN && wp == VK_UP && g_opts.sel > 0) g_opts.sel--;
        if (m == WM_KEYDOWN && wp == VK_DOWN && g_opts.sel + 1 < (int)g_opts.rates.size()) g_opts.sel++;
        if (m == WM_KEYDOWN) return 0;
        if (m == WM_PRINT || m == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC dc = m == WM_PRINT ? (HDC)wp : BeginPaint(h, &ps);
            wchar_t t[32];
            int n = wsprintfW(t, L"%d Hz", g_opts.rates[(size_t)g_opts.sel]);
            ExtTextOutW(dc, 2, 2, 0, nullptr, t, (UINT)n, nullptr);
            if (m == WM_PAINT) EndPaint(h, &ps);
            return 0;
        }
    } else if (c == L"TAIMPOptionsMenu") {
        if ((m == WM_KEYDOWN && wp == VK_HOME) || m == WM_LBUTTONDOWN) {  // first entry: Sound Output
            g_opts.Show(true);
            return 0;
        }
    } else if (c == L"TACLButton") {
        if (m == WM_KEYUP && wp == VK_SPACE && GetWindowLongPtrW(h, GWLP_ID) == 3) {  // Apply
            g_opts.applied++;
            if (g_opts.pluginsShown) g_opts.restarts++;
            g_opts.config->v["AIMPSoundOut\\DeviceFreq"] = std::to_string(g_opts.rates[(size_t)g_opts.sel]);
            return 0;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool g_noDisplay = false;
// Wine without a display creates no top-level windows
static bool CanCreateWindows() {
    HWND w = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (w) DestroyWindow(w);
    return w != nullptr;
}

// AIMP_MSG_CMD_OPTIONS: opens the preferences on the page used last, or shows the kept window again (on the
// page it shows)
static void OpenMockOptions() {
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        registered = true;
        for (const wchar_t* n : {L"TAIMPOptionsForm", L"TAIMPOptionsMenu", L"TAIMPOptionsFrameSoundOut", L"TAIMPOptionsFramePlugins",
                                 L"TACLComboBox", L"TACLButton"}) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = MockOptionsProc;
            wc.hInstance = hi;
            wc.lpszClassName = n;
            RegisterClassW(&wc);
        }
    }
    auto child = [&](HWND parent, const wchar_t* cls, int x, int y, int w, int hgt, int id) {
        return CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE, x, y, w, hgt, parent, (HMENU)(INT_PTR)id, hi, nullptr);
    };
    g_opts.opened++;
    g_opts.pluginsShown = false;
    if (!g_opts.form) {
        g_opts.form = CreateWindowExW(0, L"TAIMPOptionsForm", L"Preferences", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600,
                                      nullptr, nullptr, hi, nullptr);
        g_opts.menu = child(g_opts.form, L"TAIMPOptionsMenu", 0, 0, 180, 500, 10);
        // all pages exist from the start, only the active one is visible
        g_opts.plugins = child(g_opts.form, L"TAIMPOptionsFramePlugins", 190, 10, 590, 460, 30);
        g_opts.page = child(g_opts.form, L"TAIMPOptionsFrameSoundOut", 190, 10, 590, 460, 20);
        child(g_opts.page, L"TACLComboBox", 10, 10, 400, 22, 21);                // device (wide)
        g_opts.list = child(g_opts.page, L"TACLComboBox", 10, 40, 120, 22, 22);  // rate
        child(g_opts.page, L"TACLComboBox", 140, 40, 120, 22, 23);               // bit depth
        child(g_opts.page, L"TACLComboBox", 270, 40, 120, 22, 24);               // channels
        child(g_opts.form, L"TACLButton", 200, 480, 120, 26, 4);  // a button further up (e.g. "DSP Manager")
        child(g_opts.form, L"TACLButton", 480, 530, 90, 26, 1);   // OK
        child(g_opts.form, L"TACLButton", 580, 530, 90, 26, 2);   // Cancel
        child(g_opts.form, L"TACLButton", 680, 530, 90, 26, 3);   // Apply
        auto it = g_opts.config->v.find("Options\\ActivePage");
        g_opts.Show(it == g_opts.config->v.end() || std::atoi(it->second.c_str()) == 0);
    } else if (GetWindowLongPtrW(g_opts.plugins, GWL_STYLE) & WS_VISIBLE) {
        g_opts.pluginsShown = true;
    }
    g_opts.sel = 0;  // the page shows AIMP's current rate
    for (size_t i = 0; i < g_opts.rates.size(); i++)
        if (g_opts.rates[i] == g_opts.Current()) g_opts.sel = (int)i;
    ShowWindow(g_opts.form, SW_SHOW);
}
#endif

struct MemStream : Obj<IAIMPMemoryStream> {
    std::string d;
    INT64 pos = 0;
    MemStream() { iids = {&IID_IAIMPStream, &IID_IAIMPMemoryStream}; }
    INT64 GetSize() override { return (INT64)d.size(); }
    HRESULT SetSize(const INT64 v) override { d.resize((size_t)v); return S_OK; }
    INT64 GetPosition() override { return pos; }
    HRESULT Seek(const INT64 o, INT32 m) override { pos = m == 0 ? o : m == 1 ? pos + o : (INT64)d.size() + o; return S_OK; }
    INT32 Read(void* b, DWORD n) override {
        INT64 k = (std::min)((INT64)n, (INT64)d.size() - pos);
        if (k <= 0) return 0;
        memcpy(b, d.data() + pos, (size_t)k);
        pos += k;
        return (INT32)k;
    }
    HRESULT Write(void* b, DWORD n, DWORD* w) override {
        if ((size_t)pos + n > d.size()) d.resize((size_t)pos + n);
        memcpy(&d[(size_t)pos], b, n);
        pos += n;
        if (w) *w = n;
        return S_OK;
    }
    void* GetData() override { return d.empty() ? nullptr : &d[0]; }
};

// Picture (the author's picture on the About tab): drawn as one colour derived from its bytes - grey-ish,
// never the pink of the heart that "boop!" / "pat" show (from the Discord RP plugin's host test)
static std::atomic<int> g_imageDraws{0};
struct Image : Obj<IAIMPImage> {
    uint32_t color = 0x808080;
    Image() { iids = {&IID_IAIMPImage}; }
    HRESULT LoadFromFile(IAIMPString*) override { return E_NOTIMPL; }
    HRESULT LoadFromStream(IAIMPStream* s) override {
        std::string b;
        char buf[4096];
        INT32 n;
        while ((n = s->Read(buf, sizeof buf)) > 0) b.append(buf, (size_t)n);
        if (b.size() < 8 || memcmp(b.data(), "\x89PNG", 4) != 0) return E_FAIL;
        uint32_t h = 2166136261u;
        for (unsigned char c : b) h = (h ^ c) * 16777619u;
        color = (h & 0x7f7f7f) | 0x404040;
        return S_OK;
    }
    HRESULT SaveToFile(IAIMPString*, INT32) override { return E_NOTIMPL; }
    HRESULT SaveToStream(IAIMPStream*, INT32) override { return E_NOTIMPL; }
    INT32 GetFormatID() override { return AIMP_IMAGE_FORMAT_PNG; }
    HRESULT GetSize(SIZE* size) override { size->cx = size->cy = 128; return S_OK; }
    HRESULT Clone(IAIMPImage**) override { return E_NOTIMPL; }
    HRESULT Draw(HCANVAS canvas, RECT r, DWORD, IUnknown*) override {
        ++g_imageDraws;
#ifdef _WIN32
        HBRUSH b = CreateSolidBrush(RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
        FillRect(canvas, &r, b);
        DeleteObject(b);
#else
        cairo_rectangle(canvas, r.left, r.top, r.right - r.left, r.bottom - r.top);
        cairo_set_source_rgb(canvas, ((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0);
        cairo_fill(canvas);
#endif
        return S_OK;
    }
    HRESULT Resize(INT32, INT32) override { return S_OK; }
};

// HTTP client: answers from a URL -> content map; completion is reported on the main thread
struct HttpSvc : Obj<IAIMPServiceHTTPClient> {
    std::map<std::string, std::string> pages;
    std::vector<std::string> requested;
    HttpSvc() { iids = {&IID_IAIMPServiceHTTPClient}; }
    HRESULT Get(IAIMPString* url, DWORD, IAIMPStream* answer, IAIMPHTTPClientEvents* ev, IAIMPConfig*, TTaskHandle* task) override {
        std::string u = N(tstr(url->GetData(), url->GetLength()));
        requested.push_back(u);
        auto it = pages.find(u);
        bool found = it != pages.end();
        if (found) answer->Write((void*)it->second.data(), (DWORD)it->second.size(), nullptr);
        ev->AddRef();
        g_main.q.push_back([ev, found] { ev->OnComplete(nullptr, found ? 0 : 1); ev->Release(); });
        if (task) *task = 1;
        return S_OK;
    }
    HRESULT Post(IAIMPString*, DWORD, IAIMPStream*, IAIMPStream*, IAIMPHTTPClientEvents*, IAIMPConfig*, TTaskHandle*) override { return E_NOTIMPL; }
    HRESULT Cancel(TTaskHandle, DWORD) override { return S_OK; }
};

IAIMPString* mockui::NewString(const std::basic_string<TChar>& v) { auto* s = new Str(); s->s = v; return s; }
static mockui::Service g_ui;
static mockui::OptionsService g_options;

struct ShutdownSvc : Obj<IAIMPServiceShutdown> {
    int calls = 0, restarts = 0;
    ShutdownSvc() { iids = {&IID_IAIMPServiceShutdown}; }
    HRESULT Restart(IAIMPString*) override { restarts++; return S_OK; }
    HRESULT Shutdown(DWORD) override { calls++; return S_OK; }
};

// AIMP's language (IAIMPServiceMUI): name and LangId of the language file
static std::string g_muiName = "English (USA)", g_muiLangId = "1033";
struct Mui : Obj<IAIMPServiceMUI> {
    Mui() { iids = {&IID_IAIMPServiceMUI}; }
    HRESULT GetName(IAIMPString** v) override { *v = MkStr(g_muiName); return S_OK; }
    HRESULT GetValue(IAIMPString* key, IAIMPString** v) override {
        if (!key || N(tstr(key->GetData(), key->GetLength())) != "FILE\\LangId") return E_FAIL;
        *v = MkStr(g_muiLangId);
        return S_OK;
    }
    HRESULT GetValuePart(IAIMPString*, INT32, IAIMPString**) override { return E_FAIL; }
};

// Actions (hotkeys) and menu items of the plugin
struct Action : PropsOf<IAIMPAction> {
    Action() { iids = {&IID_IAIMPAction, &IID_IAIMPPropertyList}; }
};
struct MenuItem : PropsOf<IAIMPMenuItem> {
    MenuItem() { iids = {&IID_IAIMPMenuItem, &IID_IAIMPPropertyList}; }
    HRESULT DeleteChildren() override { return S_OK; }
};
// Runs the event an action or menu item has (click, hotkey, shown)
static bool Fire(PropsOf<IAIMPMenuItem>* m, int prop) {
    if (!m || !m->objs.count(prop)) return false;
    IAIMPActionEvent* e = nullptr;
    if (Failed(m->objs[prop]->QueryInterface(IID_IAIMPActionEvent, (void**)&e)) || !e) return false;
    e->OnExecute(nullptr);
    e->Release();
    return true;
}
static bool Fire(PropsOf<IAIMPAction>* a) {
    if (!a || !a->objs.count(AIMP_ACTION_PROPID_EVENT)) return false;
    IAIMPActionEvent* e = nullptr;
    if (Failed(a->objs[AIMP_ACTION_PROPID_EVENT]->QueryInterface(IID_IAIMPActionEvent, (void**)&e)) || !e) return false;
    e->OnExecute(nullptr);
    e->Release();
    return true;
}
struct MenuSvc : Obj<IAIMPServiceMenuManager> {
    std::map<int, MenuItem*> builtIn;  // AIMP's own menus, by id
    MenuSvc() { iids = {&IID_IAIMPServiceMenuManager}; }
    ~MenuSvc() { for (auto& kv : builtIn) kv.second->Release(); }
    HRESULT GetBuiltIn(INT32 id, IAIMPMenuItem** m) override {
        if (id != AIMP_MENUID_PLAYER_MAIN_OPTIONS && id != AIMP_MENUID_PLAYER_TRAY && id != AIMP_MENUID_COMMON_UTILITIES) return E_FAIL;
        if (!builtIn.count(id)) {
            builtIn[id] = new MenuItem();
            builtIn[id]->strs[AIMP_MENUITEM_PROPID_ID] = T("aimp.builtin." + std::to_string(id));
        }
        builtIn[id]->AddRef();
        *m = builtIn[id];
        return S_OK;
    }
    HRESULT GetByID(IAIMPString*, IAIMPMenuItem**) override { return E_FAIL; }
};
struct ActionSvc : Obj<IAIMPServiceActionManager> {
    ActionSvc() { iids = {&IID_IAIMPServiceActionManager}; }
    HRESULT GetByID(IAIMPString*, IAIMPAction**) override { return E_FAIL; }
    DWORD MakeHotkey(WORD mods, WORD key) override { return ((DWORD)mods << 16) | key; }
};

struct CoreImpl : Obj<IAIMPCore> {
    Player* player = new Player();
    Threads* threads = new Threads();
    Dispatcher* disp = new Dispatcher();
    ConfigSvc* config = new ConfigSvc();
    ShutdownSvc* shutdown = new ShutdownSvc();
    HttpSvc* http = new HttpSvc();
    Mui* mui = new Mui();
    MenuSvc* menus = new MenuSvc();
    ActionSvc* actionSvc = new ActionSvc();
    std::string profile;
    IUnknown* frame = nullptr;
    std::vector<MenuItem*> menuItems;  // registered by the plugin
    std::vector<Action*> actions;
    MenuItem* Item(const std::string& id) {
        for (auto* m : menuItems)
            if (m->S(AIMP_MENUITEM_PROPID_ID) == id) return m;
        return nullptr;
    }
    Action* ActionById(const std::string& id) {
        for (auto* a : actions)
            if (a->S(AIMP_ACTION_PROPID_ID) == id) return a;
        return nullptr;
    }
    CoreImpl() { iids = {&IID_IAIMPCore}; }
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override {
        IUnknown* s = nullptr;
        if (EqualGUID(riid, IID_IAIMPServicePlayer)) s = player;
        else if (EqualGUID(riid, IID_IAIMPServiceThreads)) s = threads;
        else if (EqualGUID(riid, IID_IAIMPServiceMessageDispatcher)) s = disp;
        else if (EqualGUID(riid, IID_IAIMPServiceConfig)) s = config;
        else if (EqualGUID(riid, IID_IAIMPServiceShutdown)) s = shutdown;
        else if (EqualGUID(riid, IID_IAIMPServiceHTTPClient)) s = http;
        else if (EqualGUID(riid, IID_IAIMPServiceMUI)) s = mui;
        else if (EqualGUID(riid, IID_IAIMPServiceUI)) s = static_cast<IAIMPServiceUI*>(&g_ui);
        else if (EqualGUID(riid, IID_IAIMPServiceOptionsDialog)) s = static_cast<IAIMPServiceOptionsDialog*>(&g_options);
        else if (EqualGUID(riid, IID_IAIMPServiceMenuManager)) s = menus;
        else if (EqualGUID(riid, IID_IAIMPServiceActionManager)) s = actionSvc;
        if (s) { s->AddRef(); *p = s; return S_OK; }
        return Obj<IAIMPCore>::QueryInterface(riid, p);
    }
    HRESULT CreateObject(CONSTIID iid, void** o) override {
        if (EqualGUID(iid, IID_IAIMPString)) { *o = new Str(); return S_OK; }
        if (EqualGUID(iid, IID_IAIMPMemoryStream)) { *o = static_cast<IAIMPMemoryStream*>(new MemStream()); return S_OK; }
        if (EqualGUID(iid, IID_IAIMPImage)) { *o = static_cast<IAIMPImage*>(new Image()); return S_OK; }
        if (EqualGUID(iid, IID_IAIMPAction)) { *o = static_cast<IAIMPAction*>(new Action()); return S_OK; }
        if (EqualGUID(iid, IID_IAIMPMenuItem)) { *o = static_cast<IAIMPMenuItem*>(new MenuItem()); return S_OK; }
        return E_NOTIMPL;
    }
    HRESULT GetPath(int, IAIMPString** v) override { *v = MkStr(profile); return S_OK; }
    HRESULT RegisterExtension(CONSTIID iid, IUnknown* e) override {
        if (EqualGUID(iid, IID_IAIMPServiceMenuManager)) {
            IAIMPMenuItem* m = nullptr;
            CHECK(Succeeded(e->QueryInterface(IID_IAIMPMenuItem, (void**)&m)) && m);
            if (!m) return E_INVALIDARG;
            auto* item = static_cast<MenuItem*>(m);
            CHECK(!item->S(AIMP_MENUITEM_PROPID_ID).empty() && item->objs.count(AIMP_MENUITEM_PROPID_PARENT));  // AIMP needs both
            CHECK(!Item(item->S(AIMP_MENUITEM_PROPID_ID)));  // ids are unique
            menuItems.push_back(item);  // keeps the reference from QueryInterface
            return S_OK;
        }
        if (EqualGUID(iid, IID_IAIMPServiceActionManager)) {
            IAIMPAction* a = nullptr;
            CHECK(Succeeded(e->QueryInterface(IID_IAIMPAction, (void**)&a)) && a);
            if (!a) return E_INVALIDARG;
            auto* action = static_cast<Action*>(a);
            CHECK(!action->S(AIMP_ACTION_PROPID_ID).empty() && !ActionById(action->S(AIMP_ACTION_PROPID_ID)));
            actions.push_back(action);
            return S_OK;
        }
        CHECK(EqualGUID(iid, IID_IAIMPServiceOptionsDialog));
        e->AddRef();
        frame = e;
        return S_OK;
    }
    HRESULT RegisterService(IUnknown*) override { return E_NOTIMPL; }
    HRESULT UnregisterExtension(IUnknown* e) override {
        if (e == frame) { frame->Release(); frame = nullptr; }
        for (size_t i = 0; i < menuItems.size(); i++)
            if (static_cast<IUnknown*>(static_cast<IAIMPMenuItem*>(menuItems[i])) == e) {
                menuItems[i]->Release();
                menuItems.erase(menuItems.begin() + (long)i);
                break;
            }
        for (size_t i = 0; i < actions.size(); i++)
            if (static_cast<IUnknown*>(static_cast<IAIMPAction*>(actions[i])) == e) {
                actions[i]->Release();
                actions.erase(actions.begin() + (long)i);
                break;
            }
        return S_OK;
    }
};

static std::string ReadFile(const std::string& p) {
    std::string r;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return r;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) r.append(buf, n);
    fclose(f);
    return r;
}

// The plugin's settings file: in AIMP's profile folder <dir> (before 2.6.1: <dir>/AIMP, APPDATA / XDG_CONFIG_HOME = <dir>)
static std::string g_ini;
static std::string IniGet(const std::string& section, const std::string& key) {
    std::string data = ReadFile(g_ini), cur;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') { cur = line.substr(1, line.size() - 2); continue; }
        size_t eq = line.find('=');
        if (cur == section && eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return "";
}
static void IniSetIn(const std::string& section, const std::string& key, const std::string& value) {
    std::string data = ReadFile(g_ini), out, cur;
    bool done = false;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') {
            if (cur == section && !done) { out += key + "=" + value + "\r\n"; done = true; }
            cur = line.substr(1, line.size() - 2);
        } else if (cur == section && line.compare(0, key.size() + 1, key + "=") == 0) {
            line = key + "=" + value;
            done = true;
        }
        out += line + "\r\n";
    }
    if (!done && cur == section) { out += key + "=" + value + "\r\n"; done = true; }
    if (!done) out += "[" + section + "]\r\n" + key + "=" + value + "\r\n";
    FILE* f = fopen(g_ini.c_str(), "wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}
static void IniSet(const std::string& key, const std::string& value) { IniSetIn("PreventResampling", key, value); }

// Waits (and pumps the main thread) until cond is true
static bool WaitUntil(std::function<bool()> cond, int ms = 3000) {
    for (int i = 0; i < ms / 5; i++) {
        g_main.Pump();
#ifdef _WIN32
        MSG msg;  // thread timers (the animation on the About tab) need a message loop, like in AIMP
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
#endif
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Layout of the settings page (same grid as the Discord RP plugin): every control at a fixed position,
// anchored top-left, inside 465 x 420 (96 DPI), no overlaps on a tab, texts fit (estimated: Segoe UI 9 pt
// is about 6 px per character, bold about 6.6, 16 px per line). Writes the layout for tests/render_layout.py.
static void CheckLayout(const std::string& out) {
    using Info = mockui::Registry::Info;
    std::vector<Info> all;
    for (auto& f : g_ui.reg.all) all.push_back(f());
    std::map<void*, std::string> sheets;
    std::vector<void*> order;
    for (auto& i : all)
        if (i.kind == mockui::Service::K("S")) {
            auto c = i.strs->find(AIMPUI_TABSHEET_PROPID_CAPTION);
            sheets[i.self] = c == i.strs->end() ? "?" : U8(c->second);
            order.push_back(i.self);
        }
    // a scroll box fills its tab (Output tab, 2.7.0): its controls belong to the tab and may go further down
    std::map<void*, void*> scrolls;  // scroll box -> its tab sheet
    for (auto& i : all)
        if (N(i.kind) == "Scroll" && sheets.count(i.parent)) {
            CHECK(i.placed && i.alignment == (int)ualClient);
            scrolls[i.self] = i.parent;
        }
    for (auto& i : all)
        if (scrolls.count(i.parent)) i.parent = scrolls[i.parent];
    CHECK(order.size() >= 3);
    printf("layout: %d objects, %d tabs\n", (int)all.size(), (int)order.size());
    FILE* f = fopen(out.c_str(), "wb");
    int controls = 0;
    for (auto& i : all) {
        if (!sheets.count(i.parent) || scrolls.count(i.self)) continue;
        controls++;
        const RECT& b = i.bounds;
        std::string kind = N(i.kind), text;
        tstr raw;
        for (int prop : {AIMPUI_LABEL_PROPID_TEXT, AIMPUI_CHECKBOX_PROPID_CAPTION, AIMPUI_BUTTON_PROPID_CAPTION})
            if (i.strs->count(prop)) raw = i.strs->at(prop);
        if (kind == "Check") raw = i.strs->count(AIMPUI_CHECKBOX_PROPID_CAPTION) ? i.strs->at(AIMPUI_CHECKBOX_PROPID_CAPTION) : tstr();
        if (kind == "Button") raw = i.strs->count(AIMPUI_BUTTON_PROPID_CAPTION) ? i.strs->at(AIMPUI_BUTTON_PROPID_CAPTION) : tstr();
        text = U8(raw);
        size_t len = Len(raw);
        bool bold = i.ints->count(AIMPUI_LABEL_PROPID_TEXTSTYLE) && kind == "Label";
        bool hidden = i.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && i.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
        std::string where = sheets[i.parent] + ": " + kind + " '" + text.substr(0, 60) + "'";
        int w = b.right - b.left, h = b.bottom - b.top;
        if (!(i.placed && i.alignment == 0 && i.anchors.left == 1 && i.anchors.top == 1)) {
            printf("layout: not anchored: %s\n", where.c_str());
            g_failures++;
        }
        bool scrolled = sheets[i.parent] == sheets[i.parent] && [&] {
            for (auto& sc : scrolls) if (sc.second == i.parent) return true;
            return false;
        }();
        if (b.left < 0 || b.top < 0 || b.right > 465 || b.bottom > (scrolled ? 4000 : 420) || w <= 0 || h <= 0) {
            printf("layout: outside 465x420: %s (%ld,%ld,%ld,%ld)\n", where.c_str(), (long)b.left, (long)b.top, (long)b.right, (long)b.bottom);
            g_failures++;
        }
        // text fits?
        bool path = !text.empty() && (text[0] == '/' || text.find(":\\") != std::string::npos);  // depends on the PC
        if (!text.empty() && !path && (kind == "Label" || kind == "Check" || kind == "Button")) {
            double cw = bold ? 6.6 : 6.0;
            int size = i.ints->count(AIMPUI_LABEL_PROPID_TEXTSIZE) ? (int)i.ints->at(AIMPUI_LABEL_PROPID_TEXTSIZE) : 0;
            if (size) cw = cw * size / 9.0;
            double avail = w - (kind == "Check" ? 20 : kind == "Button" ? 12 : 0);
            bool wrap = i.ints->count(AIMPUI_LABEL_PROPID_WORDWRAP) && i.ints->at(AIMPUI_LABEL_PROPID_WORDWRAP);
            int lines = wrap ? (int)(len * cw / (avail * 0.92)) + 1 : 1;
            bool fits = wrap ? lines * 16 <= h + 2 : len * cw <= avail;
            if (!fits) {
                printf("layout: text does not fit: %s (%d px wide, %d high)\n", where.c_str(), w, h);
                g_failures++;
            }
        }
        // combo box entries fit next to the drop-down button (device names come from the PC: not checked)
        if (kind == "Combo")
            for (auto& item : *i.items) {
                std::string t = U8(item);
                bool own = &item == &i.items->front() || (!t.empty() && t[0] == '(');  // not a device name
                if (own && Len(item) * 6.0 > w - 24) {
                    printf("layout: combo entry does not fit: %s: '%s' (%d px wide)\n", sheets[i.parent].c_str(), t.c_str(), w);
                    g_failures++;
                }
            }
        // overlaps with other visible controls on the same tab
        for (auto& o : all) {
            if (o.self == i.self || o.parent != i.parent || &o < &i) continue;
            bool oh = o.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && o.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
            if (hidden || oh) continue;
            const RECT& c = o.bounds;
            if (b.left < c.right && c.left < b.right && b.top < c.bottom && c.top < b.bottom) {
                printf("layout: overlap on %s: '%s' and '%s'\n", sheets[i.parent].c_str(), text.substr(0, 30).c_str(), N(o.kind).c_str());
                g_failures++;
            }
        }
#ifndef _WIN32
        // Linux: the Voicemeeter tab is shown, but nothing on it can be changed
        if (sheets[i.parent] == "Voicemeeter" && (kind == "Check" || kind == "Combo")) {
            bool off = i.ints->count(AIMPUI_CONTROL_PROPID_ENABLED) && i.ints->at(AIMPUI_CONTROL_PROPID_ENABLED) == 0;
            if (!off) {
                printf("layout: Voicemeeter control enabled on Linux: %s\n", where.c_str());
                g_failures++;
            }
        }
#endif
        int tab = (int)(std::find(order.begin(), order.end(), i.parent) - order.begin());
        if (f) fprintf(f, "%d\t%s\t%s\t%ld\t%ld\t%d\t%d\t%d\t%d\t%s\n", tab, sheets[i.parent].c_str(), kind.c_str(),
                       (long)b.left, (long)b.top, w, h, bold ? 1 : 0, hidden ? 1 : 0, text.c_str());
    }
    if (f) fclose(f);
    CHECK(controls > 25);
}

// Control of the settings page by kind and visible text (caption, label text or a combo box item)
static mockui::Registry::Info FindControl(const std::string& kind, const std::string& text) {
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (N(i.kind) != kind) continue;
        for (auto& kv : *i.strs)
            if (U8(kv.second) == text) return i;
        for (auto& item : *i.items)
            if (U8(item) == text) return i;
    }
    mockui::Registry::Info none{};
    none.self = nullptr;
    return none;
}
static std::string TabOf(void* parent) {
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (i.kind == mockui::Service::K("S") && i.self == parent)
            return i.strs->count(AIMPUI_TABSHEET_PROPID_CAPTION) ? U8(i.strs->at(AIMPUI_TABSHEET_PROPID_CAPTION)) : "";
    }
    return "";
}
static std::string TabCaption(int index) {
    int n = 0;
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (i.kind != mockui::Service::K("S")) continue;
        if (n++ == index) return i.strs->count(AIMPUI_TABSHEET_PROPID_CAPTION) ? U8(i.strs->at(AIMPUI_TABSHEET_PROPID_CAPTION)) : "";
    }
    return "";
}
static void WriteFile(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

// Output tab: the bit-perfect mode only works while the plugin is enabled - greyed out (with a note)
// while "Enable Prevent Resampling" is not ticked, usable again as soon as it is
static void BitPerfectNeedsPlugin() {
    auto enable = FindControl("Check", "Enable Prevent Resampling");
    auto bp = FindControl("Check", "Bit-perfect mode (switch off AIMP's sound processing)");
    auto volume = FindControl("Check", "Set AIMP's volume to 100 %");
    auto gapless = FindControl("Check", "No gapless playback (short pause between tracks)");
    CHECK(enable.self && bp.self && volume.self && gapless.self);
    if (!enable.self || !bp.self || !volume.self || !gapless.self) return;
    const char* note = "Only works while Prevent Resampling is enabled (General tab).";
    auto enabled = [](mockui::Registry::Info& i) {
        return !(i.ints->count(AIMPUI_CONTROL_PROPID_ENABLED) && i.ints->at(AIMPUI_CONTROL_PROPID_ENABLED) == 0);
    };
    auto tick = [](mockui::Registry::Info& i, bool on) {
        (*i.ints)[AIMPUI_CHECKBOX_PROPID_STATE] = on ? AIMPUI_CHECKSTATE_CHECKED : AIMPUI_CHECKSTATE_UNCHECKED;
        i.fire();
    };
    CHECK(!enabled(bp) && !enabled(volume) && !enabled(gapless));  // fresh installation: plugin off
    CHECK(FindControl("Label", note).self != nullptr);
    tick(enable, true);
    CHECK(enabled(bp) && enabled(volume) && enabled(gapless));
    CHECK(FindControl("Label", note).self == nullptr);
    tick(enable, false);
    CHECK(!enabled(bp) && !enabled(volume) && !enabled(gapless));
    CHECK(FindControl("Label", note).self != nullptr);
}

// 2.7.0 on the settings page: highest rate (F2), exceptions with the folder dialog (F3), bit depth (F5,
// Windows), the bit-perfect status and the audio chain window (U2); the timing moved to the Advanced tab
static int g_chainShown;
static void ExpectChainWindow(std::function<void(const std::vector<std::string>&)> check, const std::string& what);
static bool RenderPaint(const mockui::Registry::Info& paint, int scale, std::vector<uint32_t>& pixels);
static void NewOnThePage() {
    auto rate = FindControl("Combo", "No limit");
    CHECK(rate.self != nullptr);
    if (rate.self) {
        std::vector<std::string> items;
        for (auto& i : *rate.items) items.push_back(U8(i));
        CHECK(items.size() == 5 && items[1] == "384 kHz" && items[2] == "192 kHz" && items[3] == "96 kHz" && items[4] == "48 kHz");
        CHECK(rate.ints->count(AIMPUI_COMBOBOX_PROPID_ITEMINDEX) && rate.ints->at(AIMPUI_COMBOBOX_PROPID_ITEMINDEX) == 0);
#ifndef _WIN32
        CHECK(rate.ints->count(AIMPUI_CONTROL_PROPID_ENABLED) && rate.ints->at(AIMPUI_CONTROL_PROPID_ENABLED) == 0);  // no effect on Linux
#endif
    }
    auto lossy = FindControl("Check", "Do not switch for lossy formats (MP3, AAC, Ogg Vorbis, Opus \xE2\x80\xA6)");
    CHECK(lossy.self && (!lossy.ints->count(AIMPUI_CHECKBOX_PROPID_STATE) || lossy.ints->at(AIMPUI_CHECKBOX_PROPID_STATE) == 0));
    auto folder = FindControl("Button", "Folder...");
    CHECK(folder.self != nullptr);
    mockui::Registry::Info edit{};
    edit.self = nullptr;
    for (auto& f : g_ui.reg.all) {  // the edit left of the button
        auto i = f();
        if (N(i.kind) == "Edit" && folder.self && i.parent == folder.parent && i.bounds.top == folder.bounds.top + 1) edit = i;
    }
    CHECK(edit.self != nullptr);
    if (folder.self && edit.self) {
#ifdef _WIN32
        const std::string a = "C:\\Music\\Audiobooks", b = "D:\\Podcasts", sub = "C:\\Music\\Audiobooks\\Book 1";
#else
        const std::string a = "/music/Audiobooks", b = "/data/Podcasts", sub = "/music/Audiobooks/Book 1";
#endif
        int modified = g_options.modified;
        g_ui.folders.pick = T(a);
        folder.fire();
        CHECK(N(edit.strs->count(AIMPUI_BASEEDIT_PROPID_TEXT) ? edit.strs->at(AIMPUI_BASEEDIT_PROPID_TEXT) : tstr()) == a);
        CHECK(g_options.modified == modified + 1);
        g_ui.folders.pick = T(sub);  // already covered by its parent folder
        folder.fire();
        g_ui.folders.pick = T(b);
        folder.fire();
        CHECK(N(edit.strs->at(AIMPUI_BASEEDIT_PROPID_TEXT)) == a + "; " + b);
        g_ui.folders.pick.clear();  // cancelled
        folder.fire();
        CHECK(N(edit.strs->at(AIMPUI_BASEEDIT_PROPID_TEXT)) == a + "; " + b && g_ui.folders.opened == 4);
        (*edit.strs)[AIMPUI_BASEEDIT_PROPID_TEXT] = tstr();
    }
#ifdef _WIN32
    auto bits = FindControl("Check", "Bit depth follows the track too (16 / 24 / 32 bit, WASAPI shared)");
    CHECK(bits.self && (!bits.ints->count(AIMPUI_CHECKBOX_PROPID_STATE) || bits.ints->at(AIMPUI_CHECKBOX_PROPID_STATE) == 0));
#endif
    CHECK(FindControl("Check", "Automatic timing based on your hardware").self != nullptr);
    CHECK(FindControl("Label", "Same setting as AIMP \xE2\x86\x92 Playback \xE2\x86\x92 Output. Only changed if you pick another device here.").self == nullptr);  // a hint now
    // U2: the Output tab scrolls; below its settings the bit-perfect status (sign and line, centred) and the
    // audio chain, whose first step already peeks out at the bottom edge (420)
    mockui::Registry::Info box{}, status{}, sign{}, first{}, rail{};
    box.self = status.self = sign.self = first.self = rail.self = nullptr;
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (N(i.kind) == "Scroll") box = i;
    }
    CHECK(box.self != nullptr && box.alignment == (int)ualClient);
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (!box.self || i.parent != box.self) continue;
        bool centred = i.ints->count(AIMPUI_LABEL_PROPID_TEXTALIGN) && i.ints->at(AIMPUI_LABEL_PROPID_TEXTALIGN) == AIMPUI_ALIGN_CENTER;
        if (N(i.kind) == "Label" && centred) status = i;
        if (N(i.kind) == "Label" && i.strs->count(AIMPUI_LABEL_PROPID_TEXT) && U8(i.strs->at(AIMPUI_LABEL_PROPID_TEXT)) == "Track") first = i;
        if (N(i.kind) == "Paint" && i.bounds.right - i.bounds.left == 28 && i.bounds.bottom - i.bounds.top == 28) sign = i;
        if (N(i.kind) == "Paint" && i.bounds.bottom - i.bounds.top > 200) rail = i;
    }
    CHECK(status.self && sign.self && status.strs->count(AIMPUI_LABEL_PROPID_TEXT) && !status.strs->at(AIMPUI_LABEL_PROPID_TEXT).empty());
    CHECK(FindControl("Button", "Audio chain...").self == nullptr);  // no button any more: the chain is on the tab
    CHECK(first.self && first.bounds.top > status.bounds.top && first.bounds.top < 420);
    if (status.self && sign.self) {
        int mid = (sign.bounds.left + sign.bounds.right) / 2, smid = (status.bounds.left + status.bounds.right) / 2;
        CHECK(std::abs(mid - smid) <= 2 && sign.bounds.bottom <= status.bounds.top);
    }
    for (auto* pb : {&sign, &rail}) {
        CHECK(pb->self != nullptr);
        if (!pb->self) continue;
        std::vector<uint32_t> px;
        CHECK(RenderPaint(*pb, 2, px));
        int coloured = 0;
        for (uint32_t c : px)
            if (c != 0xF0F0F0) coloured++;
        CHECK(coloured > 100);  // a sign / the symbols of the steps were drawn
    }
}

// Advanced tab: language list (switches the page at once, every language fits the layout and has all
// texts translated), automatic language from AIMP, export and import of the settings file
static void LanguageAndSettingsFile(const std::string& dir) {
#ifdef _WIN32
    const int advanced = 4;  // General, Output, ASIO / Exclusive, Voicemeeter, Advanced, About
#else
    const int advanced = 3;
#endif
    CHECK(TabCaption(advanced) == "Advanced");
    auto lang = FindControl("Combo", "Deutsch");
    CHECK(lang.self != nullptr);
    if (!lang.self) return;
    CHECK(U8(lang.items->front()) == "Automatic (AIMP: English)" && lang.items->size() == 5);
    auto pick = [&](int index) {
        (*lang.ints)[AIMPUI_COMBOBOX_PROPID_ITEMINDEX] = index;
        lang.fire();
    };
    const char* codes[] = {"auto", "en", "de", "ru", "uk"};
    const char* captions[] = {"", "Advanced", "Erweitert", "\xD0\xA0\xD0\xB0\xD1\x81\xD1\x88\xD0\xB8\xD1\x80\xD0\xB5\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5",
                              "\xD0\xA0\xD0\xBE\xD0\xB7\xD1\x88\xD0\xB8\xD1\x80\xD0\xB5\xD0\xBD\xD1\x96"};
    for (int l = 2; l <= 4; l++) {
        pick(l);
        printf("layout in language %s:\n", codes[l]);
        CHECK(TabCaption(advanced) == captions[l]);
        CheckLayout(dir + "/layout-" + codes[l] + ".txt");
    }
    // Automatic: follows AIMP's language file (LangId 1031 = German)
    g_muiName = "Deutsch (D / A / CH)";
    g_muiLangId = "1031";
    pick(0);
    CHECK(TabCaption(advanced) == "Erweitert");
    CHECK(U8(lang.items->front()) == "Automatisch (AIMP: Deutsch)");
    g_muiName = "English (USA)";
    g_muiLangId = "1033";
    pick(1);
    pick(0);
    CHECK(TabCaption(advanced) == "Advanced");
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Missing translation") == std::string::npos);

    // Export: the settings shown on the page; ".ini" is added to a name without extension
    auto exportButton = FindControl("Button", "Export...");
    auto importButton = FindControl("Button", "Import...");
    CHECK(exportButton.self && importButton.self);
    if (!exportButton.self || !importButton.self) return;
    int shown = g_ui.message.shown;
    g_ui.dialogs.pick = T(dir + "/exported");
    remove((dir + "/exported.ini").c_str());
    exportButton.fire();
    std::string exported = ReadFile(dir + "/exported.ini");
    CHECK(exported.find("[PreventResampling]") != std::string::npos && exported.find("Language=auto") != std::string::npos &&
          exported.find("AllowMultiples=1") != std::string::npos && exported.find("[Statistics]") == std::string::npos);
    CHECK(g_ui.message.shown == shown + 1 && N(g_ui.message.text).find("exported") != std::string::npos);
    // AIMP's save dialog adds the extension a second time: "name.ini.ini" is saved as "name.ini"
    g_ui.dialogs.pick = T(dir + "/exported.ini.ini");
    remove((dir + "/exported.ini").c_str());
    exportButton.fire();
    CHECK(ReadFile(dir + "/exported.ini").find("[PreventResampling]") != std::string::npos);
    shown = g_ui.message.shown - 1;
    // Cancelled dialog: nothing happens
    g_ui.dialogs.pick.clear();
    exportButton.fire();
    importButton.fire();
    CHECK(g_ui.message.shown == shown + 1);
    // Import: shown and saved at once, the language switches
    WriteFile(dir + "/import.ini", "[PreventResampling]\r\nLanguage=ru\r\nAllowMultiples=0\r\n");
    g_ui.dialogs.pick = T(dir + "/import.ini");
    importButton.fire();
    CHECK(IniGet("PreventResampling", "Language") == "ru" && IniGet("PreventResampling", "AllowMultiples") == "0");
    CHECK(TabCaption(advanced) == captions[3]);
    CHECK(g_ui.message.shown == shown + 2);
    // A file without the plugin's settings is refused
    WriteFile(dir + "/other.ini", "[Something]\r\nLanguage=de\r\n");
    g_ui.dialogs.pick = T(dir + "/other.ini");
    importButton.fire();
    CHECK(IniGet("PreventResampling", "Language") == "ru" && g_ui.message.shown == shown + 3);
    // Back to the exported settings
    g_ui.dialogs.pick = T(dir + "/exported.ini");
    importButton.fire();
    CHECK(IniGet("PreventResampling", "Language") == "auto" && IniGet("PreventResampling", "AllowMultiples") == "1");
    CHECK(TabCaption(advanced) == "Advanced");
    g_ui.dialogs.pick.clear();
    g_ui.message.shown = 0;
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Missing translation") == std::string::npos);
}

// A paint box of the settings page drawn into pixels (0xRRGGBB, row by row) at 'scale' x its size
static bool RenderPaint(const mockui::Registry::Info& paint, int scale, std::vector<uint32_t>& pixels) {
    const int w = (paint.bounds.right - paint.bounds.left) * scale, h = (paint.bounds.bottom - paint.bounds.top) * scale;
    RECT r = {0, 0, w, h};
    pixels.clear();
#ifdef _WIN32
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));  // the skin's background
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    bool ok = paint.paint(dc, r);
    GdiFlush();
    for (int i = 0; i < w * h; ++i) pixels.push_back(static_cast<uint32_t*>(bits)[i] & 0xffffff);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
#else
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.94);  // the skin's background
    cairo_paint(cr);
    bool ok = paint.paint(cr, r);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) pixels.push_back(reinterpret_cast<const uint32_t*>(data + y * stride)[x] & 0xffffff);
    cairo_surface_destroy(surface);
#endif
    return ok;
}

// About tab: the author's picture (64 x 64, rounded corners), a quiet donation link at the right edge, and
// as in the Discord RP plugin a click on the head or the nose shows "pat" / "boop!" (white,
// with a pink heart) for about a second; clicks anywhere else, or from one area into the other, show nothing
static void AboutTab() {
    auto donate = FindControl("Label", "Support this plugin");
    CHECK(donate.self != nullptr);
    if (donate.self) {
        CHECK(N(donate.strs->count(AIMPUI_LABEL_PROPID_URL) ? donate.strs->at(AIMPUI_LABEL_PROPID_URL) : tstr()) ==
              "https://www.paypal.com/donate/?hosted_button_id=TNTR6BR72JZ6E");
        CHECK(donate.ints->count(AIMPUI_LABEL_PROPID_TEXTALIGN) && donate.ints->at(AIMPUI_LABEL_PROPID_TEXTALIGN) == AIMPUI_ALIGN_FAR);
    }
    mockui::Registry::Info av{};
    av.self = nullptr;
    for (auto& f : g_ui.reg.all) {  // the picture (other paint boxes: the status sign, the audio chain window)
        auto i = f();
        if (N(i.kind) == "Paint" && i.bounds.left == 10 && i.bounds.top == 8 && TabOf(i.parent) == "About") av = i;
    }
    CHECK(av.self != nullptr);
    if (!av.self) return;
    CHECK(av.bounds.left == 10 && av.bounds.top == 8 && av.bounds.right == 74 && av.bounds.bottom == 72);
    auto find = [&]() {  // the current state of the paint box (invalidated counts the redraws)
        for (auto& f : g_ui.reg.all) {
            auto i = f();
            if (i.self == av.self) return i;
        }
        return av;
    };
    std::vector<uint32_t> px;
    int draws = g_imageDraws;
    CHECK(RenderPaint(av, 2, px) && g_imageDraws > draws);  // the picture is drawn
    auto hearts = [&]() {  // pixels in the heart's pink (the test picture is grey-ish, never pink)
        if (!RenderPaint(av, 2, px)) return -1;
        int n = 0;
        for (uint32_t p : px) {
            const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (r > 200 && g < 150 && b > 100 && b < 210) ++n;
        }
        return n;
    };
    const float w = 64, h = 64;
    auto click = [&](float x1, float y1, float x2, float y2) {  // down at (x1, y1), up at (x2, y2): parts of the picture
        av.mouse(0, (int)(x1 * w), (int)(y1 * h));
        av.mouse(1, (int)(x1 * w), (int)(y1 * h));
        av.mouse(0, (int)(x2 * w), (int)(y2 * h));
        av.mouse(2, (int)(x2 * w), (int)(y2 * h));
        av.mouse(3, -1, -1);
    };
    CHECK(hearts() == 0);  // nothing without a click
    const float misses[][4] = {{0.05f, 0.05f, 0.05f, 0.05f}, {0.5f, 0.4f, 0.5f, 0.4f},      // corner, eyes (between)
                               {0.2f, 0.2f, 0.2f, 0.2f},     {0.76f, 0.2f, 0.76f, 0.2f},    // left / right of the head
                               {0.5f, 0.05f, 0.5f, 0.05f},   {0.28f, 0.64f, 0.28f, 0.64f},  // above, left of the nose
                               {0.5f, 0.77f, 0.5f, 0.77f},   {0.9f, 0.9f, 0.9f, 0.9f},      // under the nose, corner
                               {0.48f, 0.64f, 0.48f, 0.2f},  {0.48f, 0.2f, 0.48f, 0.64f}};  // nose -> head, head -> nose
    for (const auto& m : misses) {
        int before = find().invalidated;
        click(m[0], m[1], m[2], m[3]);
        if (find().invalidated != before || hearts() != 0) {
            printf("about: word on the picture after a click at %.2f, %.2f -> %.2f, %.2f\n", m[0], m[1], m[2], m[3]);
            g_failures++;
        }
    }
    const float hits[][2] = {{0.48f, 0.2f}, {0.48f, 0.64f}, {0.4f, 0.47f}};  // head, nose, just above the nose
    for (const auto& spot : hits) {
        int before = find().invalidated;
        click(spot[0], spot[1], spot[0], spot[1]);
        int shown = hearts();
        WaitUntil([] { return false; }, 1500);
        int frames = find().invalidated - before;
        int after = hearts();
        int idle = find().invalidated;
        WaitUntil([] { return false; }, 700);
        printf("about: picture click at %.2f, %.2f: %d heart pixels, %d frames\n", spot[0], spot[1], shown, frames);
        CHECK(shown >= 20);                          // word and heart on the picture
        CHECK(frames >= 8);                          // it moves
        CHECK(after == 0);                           // and is gone after its animation
        CHECK(find().invalidated - idle <= 1);       // the timer stopped
    }
}

static void StartTrack(CoreImpl* core, int rate) {
    core->player->info->rate = rate;
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_STREAM_START, 0, nullptr, nullptr);
}

// Internet radio: a URL is never switched (no stop, no restart); the log names only the host, never the
// path, a listening key or a password. Pausing and continuing does not log it again.
static void StreamNotSwitched(CoreImpl* core, const std::string& dir) {
    int stops = core->player->stops, plays = core->player->plays;
    size_t pw = ReadFile(dir + "/pw.log").size();
    core->player->info->file = "https://user:secret@radio.example.com:8443/live/stream.mp3?listenkey=0123abcd";
    StartTrack(core, 96000);
    const char* line = "Track 96 kHz | internet stream (https://radio.example.com:8443) - not switched";
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find(line) != std::string::npos; }));
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PAUSED, nullptr, nullptr);
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    WaitUntil([] { return false; }, 400);
    std::string log = ReadFile(dir + "/PreventResampling.log");
    CHECK(log.find(line) == log.rfind(line));
    CHECK(log.find("secret") == std::string::npos && log.find("listenkey") == std::string::npos);
    CHECK(core->player->stops == stops && core->player->plays == plays && ReadFile(dir + "/pw.log").size() == pw);
    core->player->info->file.clear();
}

// Portable AIMP (and every AIMP before 2.6.1): the settings were in %APPDATA%\AIMP (Linux: XDG_CONFIG_HOME/AIMP),
// not in AIMP's profile folder. A profile folder without a settings file takes the old file over once (copied,
// with its [Session], which is removed from the old file); afterwards the profile folder's own file is used.
static void SettingsTakeOver(CoreImpl* core, TAIMPPluginGetHeaderProc getHeader, const std::string& dir) {
    std::string portable = dir + "/portable/", legacy = dir + "/AIMP/PreventResampling.ini";
#ifdef _WIN32
    CreateDirectoryA(portable.c_str(), nullptr);
    CreateDirectoryA((dir + "/AIMP").c_str(), nullptr);
#else
    mkdir(portable.c_str(), 0755);
    mkdir((dir + "/AIMP").c_str(), 0755);
#endif
    remove((portable + "PreventResampling.ini").c_str());
    remove((portable + "PreventResampling.log").c_str());
    WriteFile(legacy, "[PreventResampling]\r\nEnabled=1\r\nLanguage=de\r\n\r\n[Session]\r\nSaved=1234\r\n\r\n"
                      "[Updates]\r\nRunVersion=2.6.0\r\n");
    std::string profile = core->profile;
    core->profile = portable;
    for (int run = 0; run < 2; run++) {
        IAIMPPlugin* plugin = nullptr;
        getHeader(&plugin);
        CHECK(Succeeded(plugin->Initialize(core)));
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
        if (run == 0) WriteFile(legacy, ReadFile(legacy) + "; changed after the takeover\r\n");
    }
    std::string now = ReadFile(portable + "PreventResampling.ini"), old = ReadFile(legacy), log = ReadFile(portable + "PreventResampling.log");
    CHECK(now.find("Language=de") != std::string::npos && now.find("Enabled=1") != std::string::npos);
#ifdef _WIN32
    // the session moved along and was taken over (at the latest when AIMP closes, so its rates are restored)
    CHECK(now.find("Saved=1234") == std::string::npos && log.find("Restart: took over") != std::string::npos);
#else
    CHECK(now.find("Saved=1234") != std::string::npos);  // the session moves along (Linux has no restart session)
#endif
    CHECK(old.find("Language=de") != std::string::npos && old.find("Saved=1234") == std::string::npos);
    size_t taken = log.find("(taken over from ");
    CHECK(taken != std::string::npos && log.find("(taken over from ", taken + 1) == std::string::npos);  // once
    CHECK(log.find("Updated from 2.6.0 to") != std::string::npos);  // the update notice still works
    core->profile = profile;
}

// ---------------------------------------------------------------- 2.7.0

// The rules of the General tab (highest rate, exceptions) and the audio chain model, tested directly
static void RulesAndChainModel() {
    using namespace ar;
    // F2: the highest rate of the track's family that is not above the limit
    CHECK(LimitRate(192000, 96000) == 96000 && LimitRate(176400, 96000) == 88200 && LimitRate(352800, 96000) == 88200);
    CHECK(LimitRate(96000, 96000) == 96000 && LimitRate(44100, 48000) == 44100 && LimitRate(192000, 0) == 192000);
    CHECK(LimitRate(384000, 192000) == 192000 && LimitRate(88200, 48000) == 44100 && LimitRate(96000, 48000) == 48000);
    CHECK(LimitRate(705600, 384000) == 352800 && LimitRate(64000, 48000) == 48000);  // no family: the limit itself
    // F3: lossy formats - AIMP's codec name first (it knows what is inside a container), then the extension
    CHECK(IsLossy(AR_T("MPEG 1 Layer 3"), AR_T("/m/a.mp3")) && IsLossy(AR_T("AAC LC"), AR_T("/m/x.m4a")));
    CHECK(!IsLossy(AR_T("ALAC"), AR_T("/m/x.m4a")) && !IsLossy(AR_T("FLAC"), AR_T("/m/x.ogg")) && IsLossy(AR_T("Ogg Vorbis"), AR_T("/m/x.ogg")));
    CHECK(IsLossy(AR_T("Opus"), AR_T("/m/x.opus")) && !IsLossy(AR_T("PCM"), AR_T("/m/x.wav")) && !IsLossy(AR_T("WavPack"), AR_T("/m/x.wv")));
    CHECK(!IsLossy(AR_T("DSD (DSF)"), AR_T("/m/x.dsf")) && !IsLossy(AR_T("Monkey's Audio"), AR_T("/m/x.ape")));
    CHECK(IsLossy(tstring(), AR_T("/m/song.MP3")) && !IsLossy(tstring(), AR_T("/m/song.flac")) && !IsLossy(tstring(), AR_T("/m.mp3/song")));
    // F3: folders (a full path covers its subfolders, a name matches anywhere) and playlist names
#ifdef _WIN32
    const tstring list = L"D:\\Audiobooks; Podcasts ;Chill-out;  ; e:/Radio plays/";
    CHECK(MatchSkipList(list, L"d:\\audiobooks\\Book 1\\01.mp3", L"") == L"D:\\Audiobooks");  // no case on Windows
    CHECK(MatchSkipList(list, L"E:\\Radio Plays\\x.flac", L"") == L"e:/Radio plays/");
    CHECK(MatchSkipList(list, L"D:\\Audiobooks2\\x.flac", L"").empty());  // only that folder, not one that starts like it
    CHECK(MatchSkipList(list, L"C:\\Music\\podcasts\\ep1.mp3", L"") == L"Podcasts");
    CHECK(MatchSkipList(list, L"C:\\Music\\Rock\\x.flac", L"chill-OUT") == L"Chill-out");
    CHECK(MatchSkipList(list, L"C:\\Music\\Rock\\x.flac", L"Rock").empty());
#else
    const tstring list = "/home/u/Audiobooks; Podcasts ;Chill-out;  ";
    CHECK(MatchSkipList(list, "/home/u/Audiobooks/Book 1/01.mp3", "") == "/home/u/Audiobooks");
    CHECK(MatchSkipList(list, "/home/u/Audiobooks2/x.flac", "").empty());
    CHECK(MatchSkipList(list, "/home/U/Audiobooks/x.mp3", "").empty());  // Linux: full paths with case, like the file system
    CHECK(MatchSkipList(list, "/music/podcasts/ep1.mp3", "") == "Podcasts");
    CHECK(MatchSkipList(list, "/music/Rock/x.flac", "chill-OUT") == "Chill-out");
    CHECK(MatchSkipList(list, "/music/Rock/x.flac", "Rock").empty());
#endif
    CHECK(MatchSkipList(tstring(), AR_T("/m/x.mp3"), AR_T("x")).empty());

    // U2: the chain for the most common cases
    auto titles = [](const ChainView& v) {
        std::string t;
        for (auto& s : v.stages) t += N(s.title) + ";";
        return t;
    };
    auto text = [](const ChainView& v) {
        std::string t;
        for (auto& s : v.stages) t += N(s.text) + "|";
        return t;
    };
    ChainInput in;
    in.enabled = true;
    in.playing = false;
    in.aimpOutput = AR_T("WASAPI: Speakers");
    ChainView v = BuildChain(in);
    CHECK(v.status == ChainStatus::Off && v.stages.front().status == ChainStatus::Off);
    CHECK(N(v.summary) == "Nothing is playing - AIMP itself does not change the sound.");
    in.changes = {"equalizer"};
    v = BuildChain(in);  // AIMP's own processing is shown without a track as well
    // the samples are changed, but nothing is resampled: yellow
    CHECK(v.status == ChainStatus::Warn && N(v.summary) == "No resampling, but not bit-perfect: AIMP equalizer");
    in.changes.clear();

    in.playing = true;
    in.file = AR_T("/music/Album/01 Song.flac");
    in.codec = AR_T("FLAC");
    in.rate = 96000;
    in.bits = 24;
    in.channels = 2;
    in.volume = 100;
#ifdef _WIN32
    in.mode = OutputMode::Shared;
    in.winKnown = true;
    in.winDevice = in.endDevice = L"Speakers (FiiO KA11)";
    in.winRate = 96000;
    in.winBits = 24;
    in.winVolume = 100;
    in.deviceRate = 96000;
    v = BuildChain(in);
    CHECK(titles(v) == "Track;AIMP sound processing;AIMP volume;AIMP output;Windows;Device;Result;");
    CHECK(v.status == ChainStatus::Ok && N(v.summary) == "Bit-perfect");
    CHECK(v.stages[0].icon == ChainIcon::Track && v.stages[4].icon == ChainIcon::System && v.stages.back().icon == ChainIcon::Result);
    CHECK(text(v).find("Sample rate: 96 kHz") != std::string::npos && text(v).find("Bit depth: 24 bit") != std::string::npos);
    CHECK(text(v).find("Default format: 96 kHz") != std::string::npos && text(v).find("Mixing: 32-bit float, samples unchanged") != std::string::npos);
    CHECK(text(v).find("Sample rate: 96 kHz (set by the plugin)") != std::string::npos && text(v).find("Type: WASAPI shared") != std::string::npos);
    CHECK(text(v).find("01 Song.flac") != std::string::npos && text(v).find("/music") == std::string::npos);  // the name, not the path
    in.winBits = 16;  // Windows' default format is 16 bit: the 24-bit track loses its lowest bits
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Warn && v.stages[4].status == ChainStatus::Bad);
    CHECK(N(v.summary).find("No resampling, but not bit-perfect: Windows reduces 24 ") == 0);
    {
        bool red = false;  // the value that changes the signal is shown in red
        for (auto& r : v.stages[4].rows) red = red || (N(r.key) == "Mixing" && r.tone == ChainStatus::Bad);
        CHECK(red);
    }
    in.winBits = 24;
    in.winRate = 48000;  // not switched (e.g. the plugin is disabled): AIMP resamples
    in.enabled = false;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Bad && v.stages[1].status == ChainStatus::Bad);
    CHECK(text(v).find("The plugin is disabled") != std::string::npos);
    in.enabled = true;
    in.winRate = 96000;
    in.limitedTo = 96000;  // a 192 kHz track with the highest rate 96 kHz: AIMP resamples, as wanted
    in.rate = 192000;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Bad && N(v.summary).find("Not bit-perfect: AIMP resamples 192 ") == 0);  // resampling is red
    in.limitedTo = 0;
    in.rate = 96000;
    in.volume = 80;
    in.crossfade = true;
    in.enhancements = 1;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Warn && text(v).find("volume 80 %") != std::string::npos &&
          text(v).find("Enhancements: may be on") != std::string::npos && v.stages[2].status == ChainStatus::Bad);
    CHECK(N(v.stages[2].text) == "Volume: 80 % (changes the samples)" && N(v.stages[1].text).find("Crossfade: at track changes") != std::string::npos);
    in.crossfadeMs = 3000;
    in.pauseMs = 500;
    in.changes = {"equalizer", "reverb", "ReplayGain"};
    in.eqMaxDb = 6;
    in.bitrate = 2304;
    v = BuildChain(in);
    CHECK(N(v.stages[1].text).find("Equalizer: on (bands up to 6.0 dB)") != std::string::npos);
    CHECK(N(v.stages[1].text).find("Sound effects: reverb") != std::string::npos && N(v.stages[1].text).find("Volume levelling: ReplayGain") != std::string::npos);
    CHECK(N(v.stages[1].text).find("Crossfade: 3 s at track changes") != std::string::npos && N(v.stages[1].text).find("Gapless: off (500 ms pause") != std::string::npos);
    CHECK(N(v.stages[0].text).find("Format: FLAC") == 0 || N(v.stages[0].text).find("2304 kbps") != std::string::npos);
    CHECK(v.status == ChainStatus::Warn && N(v.summary).find("No resampling, but not bit-perfect: AIMP equalizer") == 0);
    in.winRate = 48000;  // and resampled as well: red, the resampling is named first
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Bad && N(v.summary).find("Not bit-perfect: AIMP resamples 96 ") == 0);
    CHECK(N(v.stages.back().text).find("AIMP equalizer") != std::string::npos);
    in.winRate = 96000;
    in.changes.clear();
    in.eqMaxDb = 0;
    in.crossfadeMs = in.pauseMs = 0;
    in.bitrate = 0;
    in.volume = 100;
    in.crossfade = false;
    in.enhancements = 0;
    // Voicemeeter in between: the engine resamples if it runs at another rate
    in.vm = true;
    in.vmRate = 48000;
    in.winDevice = L"Voicemeeter Input (VB-Audio Voicemeeter VAIO)";
    in.endDevice = L"FiiO KA11";
    v = BuildChain(in);
    CHECK(titles(v) == "Track;AIMP sound processing;AIMP volume;AIMP output;Windows;Voicemeeter;Device;Result;");
    CHECK(v.status == ChainStatus::Bad && N(v.summary).find("Not bit-perfect: Voicemeeter resamples 96 ") == 0);
    in.vmRate = 96000;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Ok && text(v).find("FiiO KA11 \xC2\xB7 96 kHz") == std::string::npos);  // N() drops non-ASCII
    CHECK(N(v.stages.back().text).find("FiiO KA11 unchanged") != std::string::npos);
    in.vm = false;
    // exclusive / ASIO: Windows is not involved, AIMP's fixed rate counts
    in.mode = OutputMode::Asio;
    in.aimpRate = 96000;
    in.endDevice = L"FiiO ASIO Driver";
    v = BuildChain(in);
    CHECK(titles(v) == "Track;AIMP sound processing;AIMP volume;AIMP output;Windows;Device;Result;" && v.status == ChainStatus::Ok);
    CHECK(N(v.stages[4].text).find("Not involved") == 0 && N(v.stages[3].text).find("Sample rate: 96 kHz (fixed in AIMP)") != std::string::npos);
    // Voicemeeter installed, but AIMP plays past it: shown as not involved, still bit-perfect
    in.vmInstalled = true;
    v = BuildChain(in);
    CHECK(titles(v) == "Track;AIMP sound processing;AIMP volume;AIMP output;Windows;Voicemeeter;Device;Result;" && v.status == ChainStatus::Ok);
    CHECK(N(v.stages[5].text).find("Not involved: AIMP does not play into Voicemeeter") == 0);
    in.vmInstalled = false;
    in.aimpRate = 44100;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Bad);
    in.mode = OutputMode::Shared;
#else
    in.mode = OutputMode::PipeWire;
    in.aimpOutput = "ALSA: default";
    in.aimpRate = 48000;  // AIMP for Linux resamples to its own rate
    in.deviceRate = 48000;
    in.endDevice = "ALSA: default";
    v = BuildChain(in);
    CHECK(titles(v) == "Track;AIMP sound processing;AIMP volume;AIMP output;PipeWire;Device;Result;");
    CHECK(v.status == ChainStatus::Bad && N(v.summary).find("Not bit-perfect: AIMP resamples 96 ") == 0);
    CHECK(v.stages[4].status == ChainStatus::Ok && N(v.stages[4].text).find("like AIMP") != std::string::npos);
    CHECK(N(v.stages[4].text).find("Graph rate: 48 kHz (clock.force-rate)") == 0 && N(v.stages[1].text).find("Resampling: 96 kHz") != std::string::npos);
    in.aimpRate = 96000;
    in.deviceRate = 96000;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Ok && N(v.summary) == "Bit-perfect");
    in.deviceRate = 48000;  // PipeWire not at AIMP's rate: a second resampling
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Bad && v.stages[4].status == ChainStatus::Bad);
    in.deviceRate = 96000;
    in.aimpRate = 0;  // nothing known about the rate behind AIMP
    in.deviceRate = 0;
    v = BuildChain(in);
    CHECK(v.status == ChainStatus::Info && N(v.summary).find("Not known yet") == 0);
    in.aimpRate = in.deviceRate = 96000;
#endif
    // lossy file, internet stream, exception: noted at the file
    in.codec = AR_T("MPEG 1 Layer 3");
    in.file = AR_T("/music/x.mp3");
    in.lossy = true;
    v = BuildChain(in);
    CHECK(v.stages[0].status == ChainStatus::Info && N(v.summary) == "Bit-perfect after decoding");
    in.exception = AR_T("lossy format");
    v = BuildChain(in);
    CHECK(N(v.stages[0].text).find("Exception (General tab)") != std::string::npos);
    in.exception.clear();
    in.stream = true;
    in.file = AR_T("https://radio.example.com/live?key=1");
    v = BuildChain(in);
    CHECK(N(v.stages[0].text).find("Internet stream") != std::string::npos && N(v.stages[0].text).find("key=1") == std::string::npos);

    // Every text of the chain is translated (all cases above, in every language)
    for (int l = LangDe; l <= LangUk; l++) {
        CurrentLanguage() = l;
        ChainInput a;
        a.enabled = false;
        a.playing = false;
        BuildChain(a);
        a.changes = {"equalizer", "chorus", "echo", "flanger", "reverb", "bass boost", "pitch", "speed", "tempo", "balance",
                     "enhancer", "ReplayGain", "peak normalization", "loudness compensation", "voice remover"};
        a.volume = 50;
        a.crossfade = true;
        a.crossfadeMs = 2000;
        a.pauseMs = 500;
        a.eqMaxDb = 3;
        BuildChain(a);
        a.crossfadeMs = 0;
        a.eqMaxDb = 0;
        a.volume = -1;
        BuildChain(a);
        a.volume = 50;
        a.playing = true;
        a.file = AR_T("/m/a.flac");
        a.rate = 192000;
        a.bits = 24;
        a.channels = 2;
        a.limitedTo = 96000;
        a.aimpRate = 96000;
        a.deviceRate = 48000;
        a.winKnown = true;
        a.winFloat = false;
        a.winMuted = true;
        a.winRate = 96000;
        a.winBits = 16;
        a.winVolume = 20;
        a.enhancements = 1;
        a.vm = true;
        a.vmRate = 44100;
        for (OutputMode m : {OutputMode::Shared, OutputMode::Exclusive, OutputMode::Asio, OutputMode::DirectSound,
                             OutputMode::PipeWire, OutputMode::Direct}) {
            a.mode = m;
            BuildChain(a);
        }
        a.vm = false;
        a.vmRate = 0;
        a.winKnown = false;
        a.mode = OutputMode::Shared;
        BuildChain(a);
        a.aimpOutput = AR_T("WASAPI: x");
        a.endDevice = AR_T("x");
        a.changes.clear();
        a.volume = 100;
        a.crossfade = false;
        a.limitedTo = 0;
        a.rate = a.aimpRate = a.deviceRate = 96000;
        a.winKnown = true;
        a.winBits = 24;
        a.winVolume = 100;
        a.winMuted = false;
        a.enhancements = 0;
        a.winRate = 96000;
        a.enabled = true;
        for (OutputMode m : {OutputMode::Shared, OutputMode::PipeWire, OutputMode::Direct}) {
            a.mode = m;
            BuildChain(a);
        }
        a.rate = 48000;
        a.aimpRate = a.deviceRate = a.winRate = 96000;  // integer multiple
        BuildChain(a);
        a.vm = true;
        BuildChain(a);
        a.vmRate = 96000;
        BuildChain(a);
        a.lossy = true;
        a.vm = false;
        BuildChain(a);
        a.exception = AR_T("x");
        BuildChain(a);
        a.stream = true;
        BuildChain(a);
        a.aimpRate = a.deviceRate = 0;
        a.winKnown = false;
        a.mode = OutputMode::PipeWire;
        BuildChain(a);
        a.aimpOutput.clear();
        a.endDevice.clear();
        BuildChain(a);
    }
    CurrentLanguage() = LangEn;
    for (auto& m : MissingTranslations()) printf("chain: missing translation: %s\n", m.first.c_str());
    CHECK(MissingTranslations().empty());

    // The built-in texts are stored gzip-compressed: both unpack, every entry has all four languages, no key twice
    printf("texts: %zu translations, changelog %zu bytes (compressed %zu + %zu bytes)\n", Translations().size(),
           ChangelogSource().size(), sizeof kTranslationsGz, sizeof kChangelogGz);
    CHECK(Translations().size() > 200);
    std::map<std::string, int> keys;
    for (auto& t : Translations()) {
        for (auto& s : t) CHECK(!s.empty());
        if (++keys[t[LangEn]] > 1) printf("texts: translated twice: %s\n", t[LangEn].c_str());
        CHECK(keys[t[LangEn]] == 1);
    }
    CHECK(ChangelogSource().find("## 2.7.0") != std::string::npos);
    CHECK(ChangelogSource().find("2.6.1") != std::string::npos);
    std::string bad;
    CHECK(!inflate::Gunzip(kChangelogGz, sizeof kChangelogGz / 2, bad));  // a cut-off text is detected
}

// The controls of a window (not a tab of the settings page): inside w x h, no overlaps, texts fit
static int CheckWindowLayout(void* form, int w, int h, const std::string& what) {
    using Info = mockui::Registry::Info;
    std::vector<Info> all;
    std::map<void*, int> boxW;  // scroll boxes of the window: their content may be higher than the window
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (i.parent == form) all.push_back(i);
        if (i.parent == form && N(i.kind) == "Scroll") boxW[i.self] = i.bounds.right - i.bounds.left;
    }
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        if (boxW.count(i.parent)) all.push_back(i);
    }
    int controls = 0;
    for (auto& i : all) {
        bool hidden = i.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && i.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
        if (hidden) continue;
        controls++;
        const RECT& b = i.bounds;
        std::string kind = N(i.kind);
        tstr raw;
        if (kind == "Label" && i.strs->count(AIMPUI_LABEL_PROPID_TEXT)) raw = i.strs->at(AIMPUI_LABEL_PROPID_TEXT);
        if (kind == "Button" && i.strs->count(AIMPUI_BUTTON_PROPID_CAPTION)) raw = i.strs->at(AIMPUI_BUTTON_PROPID_CAPTION);
        std::string text = U8(raw), where = what + ": " + kind + " '" + text.substr(0, 50) + "'";
        int cw = b.right - b.left, ch = b.bottom - b.top;
        bool inBox = boxW.count(i.parent) > 0;
        int maxW = inBox ? boxW[i.parent] - 16 : w, maxH = inBox ? 4000 : h;  // room for the scroll bar
        if (!i.placed || b.left < 0 || b.top < 0 || b.right > maxW || b.bottom > maxH || cw <= 0 || ch <= 0) {
            printf("layout: outside %dx%d: %s (%ld,%ld,%ld,%ld)\n", w, h, where.c_str(), (long)b.left, (long)b.top, (long)b.right, (long)b.bottom);
            g_failures++;
        }
        if (!text.empty()) {
            bool bold = i.ints->count(AIMPUI_LABEL_PROPID_TEXTSTYLE) && kind == "Label";
            double c = bold ? 6.6 : 6.0, avail = cw - (kind == "Button" ? 12 : 0);
            bool wrap = i.ints->count(AIMPUI_LABEL_PROPID_WORDWRAP) && i.ints->at(AIMPUI_LABEL_PROPID_WORDWRAP);
            int lines = 0;  // explicit line breaks count as well
            size_t start = 0;
            for (;;) {
                size_t nl = raw.find((TChar)'\n', start);
                tstr part = raw.substr(start, nl == tstr::npos ? tstr::npos : nl - start);
                lines += wrap ? (int)(Len(part) * c / (avail * 0.92)) + 1 : 1;
                if (!wrap && Len(part) * c > avail) lines += 99;
                if (nl == tstr::npos) break;
                start = nl + 1;
            }
            if (lines * 16 > ch + 2) {
                printf("layout: text does not fit: %s (%d px wide, %d high, %d lines)\n", where.c_str(), cw, ch, lines);
                g_failures++;
            }
        }
        for (auto& o : all) {
            if (&o <= &i || o.parent != i.parent) continue;
            bool oh = o.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && o.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
            const RECT& c = o.bounds;
            if (!oh && b.left < c.right && c.left < b.right && b.top < c.bottom && c.top < b.bottom) {
                printf("layout: overlap in %s: '%s' and %s\n", what.c_str(), text.substr(0, 30).c_str(), N(o.kind).c_str());
                g_failures++;
            }
        }
    }
    return controls;
}

// Texts of the labels in a window, top to bottom
static std::vector<std::string> WindowTexts(void* form) {
    std::vector<std::pair<long, std::string>> t;
    for (auto& f : g_ui.reg.all) {
        auto i = f();
        bool hidden = i.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && i.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
        bool inForm = i.parent == form;
        if (!inForm)  // or in a scroll box of the window
            for (auto& g : g_ui.reg.all) {
                auto p = g();
                if (p.self == i.parent && p.parent == form && N(p.kind) == "Scroll") inForm = true;
            }
        if (!inForm || hidden || N(i.kind) != "Label" || !i.strs->count(AIMPUI_LABEL_PROPID_TEXT)) continue;
        t.push_back({(long)i.bounds.top, U8(i.strs->at(AIMPUI_LABEL_PROPID_TEXT))});
    }
    std::stable_sort(t.begin(), t.end(), [](const std::pair<long, std::string>& a, const std::pair<long, std::string>& b) { return a.first < b.first; });
    std::vector<std::string> r;
    for (auto& x : t) r.push_back(x.second);
    return r;
}

// The audio chain window: opened without a modal loop (AIMP stays usable), shows the steps, follows the language, fits its layout,
// Refresh and Close work. check(texts) looks at the steps.
static void ExpectChainWindow(std::function<void(const std::vector<std::string>&)> check, const std::string& what) {
    g_ui.reg.onModal = [check, what](void* form) {
        g_chainShown++;
        auto* f = g_ui.lastForm;
        CHECK(f && static_cast<IAIMPUIControl*>(static_cast<IAIMPUIForm*>(f)) == form);
        if (f) CHECK(N(f->name) == "PreventResamplingChain" && f->strs.count(AIMPUI_FORM_PROPID_CAPTION) &&
                     f->ints.count(AIMPUI_FORM_PROPID_CLIENTWIDTH) && f->ints.count(AIMPUI_FORM_PROPID_CLIENTHEIGHT));
        int w = f ? (int)f->ints[AIMPUI_FORM_PROPID_CLIENTWIDTH] : 0, h = f ? (int)f->ints[AIMPUI_FORM_PROPID_CLIENTHEIGHT] : 0;
        CHECK(CheckWindowLayout(form, w, h, "audio chain (" + what + ")") >= 8);
        auto texts = WindowTexts(form);
        if (check) check(texts);
        // Refresh and Close
        for (auto& fn : g_ui.reg.all) {
            auto i = fn();
            if (i.parent != form || N(i.kind) != "Button") continue;
            i.fire();
        }
        CHECK(f && f->closed);
    };
}

static std::string Joined(const std::vector<std::string>& v) {
    std::string r;
    for (auto& s : v) r += s + "\n";
    return r;
}

// F1, F3, B5 and U2 in a run of their own (they switch the plugin on and off and play tracks)
static void QuickSwitchRun(CoreImpl* core, TAIMPPluginGetHeaderProc getHeader, const std::string& dir) {
    std::string logFile = dir + "/PreventResampling.log";
    IniSet("Enabled", "0");
    IniSet("Language", "auto");
    IniSet("Mode", "0");
    IniSet("LegacyRestart", "0");
    core->player->state = AIMP_PLAYER_STATE_STOPPED;
    int notes = core->disp->notifications;
    IAIMPPlugin* plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));

    // --- F1: two actions for hotkeys (no default keys) and the same menu in AIMP's main menu and tray menu
    CHECK(core->actions.size() == 2);
    auto* toggle = core->ActionById("PreventResampling.Toggle");
    auto* chain = core->ActionById("PreventResampling.Chain");
    CHECK(toggle && chain);
    if (!toggle || !chain) { plugin->Finalize(); plugin->Release(); return; }
    CHECK(toggle->S(AIMP_ACTION_PROPID_NAME) == "Switch on / off" && chain->S(AIMP_ACTION_PROPID_NAME) == "Show the audio chain");
    CHECK(toggle->S(AIMP_ACTION_PROPID_GROUPNAME) == "Prevent Resampling");
    CHECK(!toggle->ints.count(AIMP_ACTION_PROPID_DEFAULTLOCALHOTKEY) && !toggle->ints.count(AIMP_ACTION_PROPID_DEFAULTGLOBALHOTKEY));
    CHECK(core->menuItems.size() == 8);
    for (const char* root : {"PreventResampling.Menu", "PreventResampling.Tray"}) {
        std::string r = root;
        auto* m = core->Item(r);
        CHECK(m && m->S(AIMP_MENUITEM_PROPID_CAPTION) == "Prevent Resampling");
        if (!m) continue;
        IUnknown* parent = m->objs[AIMP_MENUITEM_PROPID_PARENT];
        int builtIn = r == "PreventResampling.Menu" ? AIMP_MENUID_PLAYER_MAIN_OPTIONS : AIMP_MENUID_PLAYER_TRAY;
        CHECK(parent == static_cast<IUnknown*>(static_cast<IAIMPMenuItem*>(core->menus->builtIn[builtIn])));
        auto* en = core->Item(r + ".Enabled");
        auto* ch = core->Item(r + ".Chain");
        auto* st = core->Item(r + ".Settings");
        CHECK(en && ch && st);
        if (!en || !ch || !st) continue;
        CHECK(en->objs[AIMP_MENUITEM_PROPID_PARENT] == static_cast<IUnknown*>(static_cast<IAIMPMenuItem*>(m)));
        CHECK(en->S(AIMP_MENUITEM_PROPID_CAPTION) == "Enabled" && en->ints[AIMP_MENUITEM_PROPID_STYLE] == AIMP_MENUITEM_STYLE_CHECKBOX);
        CHECK(en->ints[AIMP_MENUITEM_PROPID_CHECKED] == 0);  // the plugin is off
        CHECK(ch->S(AIMP_MENUITEM_PROPID_CAPTION) == "Audio chain..." && st->S(AIMP_MENUITEM_PROPID_CAPTION) == "Settings...");
    }
    auto* menuEnabled = core->Item("PreventResampling.Menu.Enabled");
    auto* trayEnabled = core->Item("PreventResampling.Tray.Enabled");
    if (!menuEnabled || !trayEnabled) { plugin->Finalize(); plugin->Release(); return; }
    // the hotkey switches the plugin on: saved, both menus ticked, AIMP shows it for 2 s
    CHECK(Fire(toggle));
    CHECK(IniGet("PreventResampling", "Enabled") == "1");
    CHECK(menuEnabled->ints[AIMP_MENUITEM_PROPID_CHECKED] == 1 && trayEnabled->ints[AIMP_MENUITEM_PROPID_CHECKED] == 1);
    CHECK(core->disp->notifications == notes + 1 && N(core->disp->notification) == "Prevent Resampling is on");
    CHECK(ReadFile(logFile).find("Plugin enabled (hotkey)") != std::string::npos);
    // the menu switches it off again
    CHECK(Fire(trayEnabled, AIMP_MENUITEM_PROPID_EVENT));
    CHECK(IniGet("PreventResampling", "Enabled") == "0");
    CHECK(menuEnabled->ints[AIMP_MENUITEM_PROPID_CHECKED] == 0 && trayEnabled->ints[AIMP_MENUITEM_PROPID_CHECKED] == 0);
    CHECK(N(core->disp->notification) == "Prevent Resampling is off");
    CHECK(ReadFile(logFile).find("Plugin disabled (menu)") != std::string::npos);
    // switched on the settings page: the menu shows it the next time it opens
    IniSet("Enabled", "1");  // (as if saved by the page of another instance - the menu reads the plugin's state)
    CHECK(Fire(core->Item("PreventResampling.Menu"), AIMP_MENUITEM_PROPID_EVENT_ONSHOW));
    CHECK(menuEnabled->ints[AIMP_MENUITEM_PROPID_CHECKED] == 0);  // the running plugin is still off
    CHECK(Fire(menuEnabled, AIMP_MENUITEM_PROPID_EVENT));         // on again (from the menu)
    CHECK(menuEnabled->ints[AIMP_MENUITEM_PROPID_CHECKED] == 1);
    // "Settings..." opens the plugin's page in AIMP's preferences
    int pages = g_options.shown;
    CHECK(Fire(core->Item("PreventResampling.Menu.Settings"), AIMP_MENUITEM_PROPID_EVENT) && g_options.shown == pages + 1);
    // AIMP's language changes: the menus follow (the plugin's language is "automatic")
    g_muiName = "Deutsch (D / A / CH)";
    g_muiLangId = "1031";
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LANGUAGE, 0, nullptr, nullptr);
    CHECK(menuEnabled->S(AIMP_MENUITEM_PROPID_CAPTION) == "Aktiviert" && U8(T(toggle->S(AIMP_ACTION_PROPID_NAME))) != "Switch on / off");
    g_muiName = "English (USA)";
    g_muiLangId = "1033";
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LANGUAGE, 0, nullptr, nullptr);
    CHECK(menuEnabled->S(AIMP_MENUITEM_PROPID_CAPTION) == "Enabled");

    // --- U2: the audio chain window from the menu, nothing playing
    int shown = g_chainShown, loops = g_ui.reg.modalLoops;
    ExpectChainWindow([](const std::vector<std::string>& t) {
        CHECK(t.size() >= 8 && t[1] == "Track" && t[2] == "Nothing is playing.");
        CHECK(Joined(t).find("Start a track to see its way to your device.") != std::string::npos);
    }, "nothing playing");
    CHECK(Fire(core->Item("PreventResampling.Menu.Chain"), AIMP_MENUITEM_PROPID_EVENT) && g_chainShown == shown + 1);
    CHECK(ReadFile(logFile).find("Audio chain window opened") != std::string::npos);
    CHECK(g_ui.reg.modalLoops == loops);  // not modal: AIMP can be used while it is open
    CHECK(ReadFile(logFile).find("Audio chain window closed") != std::string::npos);

    // --- F3: exceptions - the output stays as it is (no switch, no stop)
    core->player->state = AIMP_PLAYER_STATE_PLAYING;
#ifndef _WIN32
    FILE* ini = fopen((dir + "/AIMP.ini").c_str(), "wb");
    fprintf(ini, "\xEF\xBB\xBF[System]\nVersion=6000\n[AIMPSoundOut]\nDeviceName=ALSA: default\nDeviceFreq=48000\n");
    fclose(ini);
    core->config->v["AIMPSoundOut\\DeviceFreq"] = "48000";
#endif
    plugin->Finalize();
    plugin->Release();
    IniSet("SkipLossy", "1");
    IniSet("SkipFoldersPlaylists", "Podcasts; Radio plays");
    IniSet("MaxRate", "96000");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    int stops = core->player->stops;
    size_t pw = ReadFile(dir + "/pw.log").size();
    core->player->info->codec = "MPEG 1 Layer 3";
    core->player->info->file = "/music/mp3/song.mp3";
    StartTrack(core, 44100);
    CHECK(WaitUntil([&] { return ReadFile(logFile).find("Track 44.1 kHz | MPEG 1 Layer 3 | not switched: lossy format") != std::string::npos; }));
    // the chain window names the exception
    ExpectChainWindow([](const std::vector<std::string>& t) {
        CHECK(t.size() >= 3 && t[2].find("song.mp3") == 0);
        CHECK(Joined(t).find("Exception (General tab)") != std::string::npos);
    }, "exception");
    CHECK(Fire(core->ActionById("PreventResampling.Chain")));
    core->player->info->codec = "FLAC";
    core->player->info->file = "/music/Podcasts/episode 1.flac";
    StartTrack(core, 48000);
    CHECK(WaitUntil([&] { return ReadFile(logFile).find("not switched: exception list (Podcasts)") != std::string::npos; }));
    core->player->info->file = "/music/Album/01.flac";
    core->player->playlist = "radio PLAYS";
    StartTrack(core, 88200);
    CHECK(WaitUntil([&] { return ReadFile(logFile).find("not switched: exception list (Radio plays)") != std::string::npos; }));
    core->player->playlist.clear();
    WaitUntil([] { return false; }, 300);
    CHECK(core->player->stops == stops && ReadFile(dir + "/pw.log").size() == pw);
#ifndef _WIN32
    // --- F2 on Linux: PipeWire follows AIMP's own rate - the highest rate setting must not lower it
    ini = fopen((dir + "/AIMP.ini").c_str(), "wb");
    fprintf(ini, "\xEF\xBB\xBF[System]\nVersion=6000\n[AIMPSoundOut]\nDeviceName=ALSA: default\nDeviceFreq=192000\n");
    fclose(ini);
    core->config->v["AIMPSoundOut\\DeviceFreq"] = "192000";
    core->player->info->file.clear();
    StartTrack(core, 192000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/pw.log").find("clock.force-rate 192000", pw) != std::string::npos; }));
    CHECK(ReadFile(dir + "/pw.log").find("clock.force-rate 96000", pw) == std::string::npos);

    // --- B5: AIMP's output rate changes while the track plays -> PipeWire follows at once
    core->config->v["AIMPSoundOut\\DeviceFreq"] = "96000";
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION, 0, nullptr, nullptr);
    CHECK(WaitUntil([&] { return ReadFile(logFile).find("Output changed (AIMP's output rate 96 kHz) - checking the current track again") != std::string::npos; }));
    CHECK(WaitUntil([&] { return ReadFile(dir + "/pw.log").find("clock.force-rate 96000", pw) != std::string::npos; }));
    // ... and AIMP's output device
    core->player->output = "PulseAudio: USB DAC";
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION, 0, nullptr, nullptr);
    CHECK(WaitUntil([&] { return ReadFile(logFile).find("Output changed (AIMP output PulseAudio: USB DAC)") != std::string::npos; }));
    // the chain window shows the way of the playing track - once the plugin has checked it again (the check runs
    // on the plugin's thread and may take a moment on a busy machine: the window is opened again until then)
    bool chainOk = false;
    std::string chainSeen;
    for (int i = 0; i < 30 && !chainOk; i++) {
        WaitUntil([] { return false; }, 200);
        ExpectChainWindow([&](const std::vector<std::string>& t) {
            std::string all = Joined(t);
            chainSeen = all;
            chainOk = t.size() >= 10 && t[1] == "Track" && all.find("\nAIMP sound processing\n") != std::string::npos &&
                      all.find("\nPipeWire\n") != std::string::npos && all.find("\nDevice\n") != std::string::npos &&
                      all.find("\nResult\n") != std::string::npos && all.find("Runs at 96 kHz like AIMP") != std::string::npos &&
                      all.find("Not bit-perfect: AIMP resamples 192 kHz \xE2\x86\x92 96 kHz") != std::string::npos;  // AIMP's own rate
        }, "playing");
        CHECK(Fire(core->ActionById("PreventResampling.Chain")));
    }
    CHECK(chainOk);
    if (!chainOk) printf("Linux chain window while playing:\n%s", chainSeen.c_str());
    // a device that keeps changing: checked at most 3 times per track
    for (int i = 0; i < 4; i++) {
        core->player->output = "PulseAudio: DAC " + std::to_string(i);
        core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION, 0, nullptr, nullptr);
        WaitUntil([] { return false; }, 150);
    }
    CHECK(WaitUntil([&] { return ReadFile(logFile).find("not checked again for this track") != std::string::npos; }));
    // --- F1 on Linux: switched off by hotkey, PipeWire's forced rate is reset meanwhile (another program or a
    // restart of PipeWire), switched on again: the plugin reads PipeWire's real rate and sets it again
    auto logCount = [&](const std::string& s) {
        std::string l = ReadFile(logFile);
        size_t n = 0;
        for (size_t p = l.find(s); p != std::string::npos; p = l.find(s, p + 1)) n++;
        return n;
    };
    size_t offs = logCount("Plugin disabled (hotkey)"), ons = logCount("Plugin enabled (hotkey)");
    CHECK(Fire(core->ActionById("PreventResampling.Toggle")));
    CHECK(WaitUntil([&] { return logCount("Plugin disabled (hotkey)") == offs + 1; }));
    if (FILE* f = fopen((dir + "/pw.log.rate").c_str(), "wb")) {
        fputs("0\n", f);
        fclose(f);
    }
    size_t pwAt = ReadFile(dir + "/pw.log").size();
    CHECK(Fire(core->ActionById("PreventResampling.Toggle")));
    CHECK(WaitUntil([&] { return logCount("Plugin enabled (hotkey)") == ons + 1; }));
    CHECK(WaitUntil([&] { return ReadFile(dir + "/pw.log").find("clock.force-rate 96000", pwAt) != std::string::npos; }));
    core->player->output = "ALSA: default";
    core->config->v.erase("AIMPSoundOut\\DeviceFreq");
    remove((dir + "/AIMP.ini").c_str());
#endif
    // --- U2 with the language of the plugin (German): every text of the window translated
    plugin->Finalize();  // saves the settings: the language is set after it
    plugin->Release();
    plugin = nullptr;
    IniSet("Language", "de");
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    ExpectChainWindow([](const std::vector<std::string>& t) {
        CHECK(t.size() >= 2 && t[1] == "Titel");
        if (t.size() < 2 || t[1] != "Titel") printf("German chain window:\n%s", Joined(t).c_str());
    }, "German");
    CHECK(Fire(core->ActionById("PreventResampling.Chain")));
    CHECK(ReadFile(logFile).find("Missing translation") == std::string::npos);
    g_ui.reg.onModal = nullptr;
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(core->menuItems.empty() && core->actions.empty());  // everything unregistered
    plugin->Release();
    core->player->info->file.clear();
    core->player->info->codec = "FLAC";
    IniSet("Language", "auto");
    IniSet("SkipLossy", "0");
    IniSet("SkipFoldersPlaylists", "");
    IniSet("MaxRate", "0");
}

#ifdef _WIN32
// B6: Voicemeeter Banana, simulated by tests/fakevm/FakeVoicemeeterRemote.c and registered like the real
// Voicemeeter (Uninstall key with the folder of VoicemeeterRemote64.dll, see .github/workflows/main.yml).
// AIMP plays into "Voicemeeter Input (VB-Audio Voicemeeter VAIO)": Windows needs a device of that name (in
// the build test a PulseAudio null sink, which Wine shows as an audio device); A1 is "FiiO KA11". Runs in a
// process of its own ("MockHost <plugin> <folder> voicemeeter"): the plugin looks for Voicemeeter only once.
static void VoicemeeterRun(CoreImpl* core, TAIMPPluginGetHeaderProc getHeader, const std::string& dir) {
    const std::string state = "C:\\vmfake.ini", vmLog = "C:\\vmfake.log", logFile = dir + "/PreventResampling.log";
    auto has = [](const std::string& file, const std::string& text) { return ReadFile(file).find(text) != std::string::npos; };
    auto count = [](const std::string& file, const std::string& text) {
        std::string all = ReadFile(file);
        int n = 0;
        for (size_t p = all.find(text); p != std::string::npos; p = all.find(text, p + 1)) n++;
        return n;
    };
    // Voicemeeter's state changes "in Voicemeeter" (the fake reads its file on every call)
    auto setState = [&](const std::string& key, const std::string& value) {
        std::string all = ReadFile(state), out;
        size_t pos = 0;
        bool done = false;
        while (pos < all.size()) {
            size_t end = all.find('\n', pos);
            if (end == std::string::npos) end = all.size();
            std::string line = all.substr(pos, end - pos);
            pos = end + 1;
            if (line.compare(0, key.size() + 1, key + "=") == 0) { line = key + "=" + value; done = true; }
            if (!line.empty()) out += line + "\n";
        }
        if (!done) out += key + "=" + value + "\n";
        WriteFile(state, out);
    };
    WriteFile(state, "running=1\nsr=48000\nasiosr=0\nengine=48000\n");
    remove(vmLog.c_str());
    IniSet("Enabled", "1");
    IniSet("RestoreOnExit", "1");
    IniSet("MaxRate", "96000");
    core->player->output = "WASAPI: Voicemeeter Input (VB-Audio Voicemeeter VAIO)";
    core->player->info->file = "C:\\Music\\Hi-Res\\track.flac";
    core->player->state = AIMP_PLAYER_STATE_PLAYING;
    IAIMPPlugin* plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    WaitUntil([] { return false; }, 300);

    // F2: a 192 kHz track runs Voicemeeter at 96 kHz (AIMP resamples 192 -> 96), A1 follows on ASIO
    StartTrack(core, 192000);
    CHECK(WaitUntil([&] { return has(logFile, "Voicemeeter: engine restarted with 96 kHz"); }, 10000));
    CHECK(has(logFile, "Voicemeeter Banana") && has(logFile, "limited to 96 kHz (highest rate setting)"));
    CHECK(has(vmLog, "SetFloat Option.sr = 96000") && has(vmLog, "SetFloat Option.ASIOsr = 1"));
    CHECK(!has(vmLog, "SetFloat Option.sr = 192000"));
    // AIMP starts the track again after the switch (AIMP_MSG_EVENT_STREAM_START). Wine cannot change the format of
    // the devices, so the plugin finds "no effect" - Voicemeeter must still be watched afterwards (B5)
    WaitUntil([] { return false; }, 600);
    StartTrack(core, 192000);
    CHECK(WaitUntil([&] { return has(logFile, "Switching to 96 kHz has no effect - skipped for this track"); }, 5000));
    WaitUntil([] { return false; }, 1000);  // the switch is finished (settle time, playback continues)
    // U2: the chain shows Voicemeeter and the device on A1
    ExpectChainWindow([](const std::vector<std::string>& t) {
        std::string all = Joined(t);
        bool ok = all.find("Voicemeeter\n") != std::string::npos && all.find("Engine rate\n") != std::string::npos &&
                  all.find("\nFiiO KA11\n") != std::string::npos && all.find("96 kHz\n") != std::string::npos;
        CHECK(ok);
        if (!ok) printf("Voicemeeter chain:\n%s", all.c_str());
    }, "Voicemeeter");
    int shown = g_chainShown;
    CHECK(Fire(core->ActionById("PreventResampling.Chain")) && g_chainShown == shown + 1);

    // B5: another device on A1 in Voicemeeter - the track is checked again right away (no new track needed)
    setState("bus0name", "Speakers (USB DAC)");
    CHECK(WaitUntil([&] { return has(logFile, "Output changed (Voicemeeter output device) - checking the current track again"); }, 8000));
    WaitUntil([] { return false; }, 1500);
    ExpectChainWindow([](const std::vector<std::string>& t) {
        std::string all = Joined(t);
        CHECK(all.find("Speakers (USB DAC)") != std::string::npos && all.find("FiiO KA11") == std::string::npos);
    }, "Voicemeeter, new A1");
    CHECK(Fire(core->ActionById("PreventResampling.Chain")) && g_chainShown == shown + 2);
    g_ui.reg.onModal = nullptr;

    // The next track (44.1 kHz), started again by AIMP after the switch as above
    StartTrack(core, 44100);
    CHECK(WaitUntil([&] { return has(logFile, "Voicemeeter: engine restarted with 44.1 kHz"); }, 10000));
    WaitUntil([] { return false; }, 600);
    StartTrack(core, 44100);
    WaitUntil([] { return false; }, 1500);
    // ... and someone sets Voicemeeter's engine to 48 kHz: the plugin puts it back to the track's 44.1 kHz
    setState("sr", "48000");
    setState("engine", "48000");
    CHECK(WaitUntil([&] { return has(logFile, "Output changed (Voicemeeter engine rate 48 kHz) - checking the current track again"); }, 8000));
    CHECK(WaitUntil([&] { return count(logFile, "Voicemeeter: engine restarted with 44.1 kHz") == 2; }, 10000));
    CHECK(count(vmLog, "SetFloat Option.sr = 44100") == 2);
    WaitUntil([] { return false; }, 2500);  // quiet now: nothing changed, nothing checked again
    CHECK(count(logFile, "Output changed (") == 2);

    // F1: switched off by hotkey, Voicemeeter's engine is set to 48 kHz meanwhile, switched on again: the plugin
    // compares with Voicemeeter's real rate (not with the rate it set before) and switches the track again
    CHECK(Fire(core->ActionById("PreventResampling.Toggle")));
    CHECK(WaitUntil([&] { return has(logFile, "Plugin disabled (hotkey)"); }));
    setState("sr", "48000");
    setState("engine", "48000");
    WaitUntil([] { return false; }, 2500);
    CHECK(count(logFile, "Output changed (") == 2);  // the plugin is off: nothing is checked
    CHECK(Fire(core->ActionById("PreventResampling.Toggle")));
    CHECK(WaitUntil([&] { return has(logFile, "Plugin enabled (hotkey)"); }));
    CHECK(WaitUntil([&] { return count(logFile, "Voicemeeter: engine restarted with 44.1 kHz") == 3; }, 10000));
    CHECK(core->disp->notifications >= 2 && N(core->disp->notification) == "Prevent Resampling is on");
    WaitUntil([] { return false; }, 1000);

    // Restore on exit: the engine rate of before (48 kHz) and Option.ASIOsr (0), then the logout
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    CHECK(has(logFile, "Voicemeeter: engine rate restored to 48 kHz") && has(logFile, "Option.ASIOsr back to 0"));
    CHECK(has(vmLog, "Engine restart: 44100 -> 48000") && has(vmLog, "Logout (engine 48000, Option.sr 48000)"));
    CHECK(ReadFile(state).find("asiosr=0") != std::string::npos);
    printf("---- PreventResampling.log (Voicemeeter) ----\n%s---- vmfake.log ----\n%s---------------------\n",
           ReadFile(logFile).c_str(), ReadFile(vmLog).c_str());
}
#endif

int main(int argc, char** argv) {
    if (argc < 3) { printf("Usage: MockHost <plugin> <work folder> [voicemeeter]\n"); return 2; }
    std::string dir = argv[2];
    // SHA-256 test vectors (FIPS 180-2): "abc" and the 56-byte two-block message
    CHECK(ar::Sha256::Of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ar::Sha256::Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    RulesAndChainModel();
    std::string pwLog = dir + "/pw.log";
    remove(pwLog.c_str());
    remove((pwLog + ".rate").c_str());  // the fake pw-metadata's clock.force-rate
    remove((dir + "/PreventResampling.log").c_str());
    remove((dir + "/AIMP.ini").c_str());
    remove((dir + "/PreventResampling.resume").c_str());
    g_ini = dir + "/PreventResampling.ini";
    remove((dir + "/AIMP/PreventResampling.ini").c_str());
    remove(g_ini.c_str());
#ifdef _WIN32
    SetEnvironmentVariableA("APPDATA", dir.c_str());
#else
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
#endif

#ifdef _WIN32
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    auto getHeader = (TAIMPPluginGetHeaderProc)GetProcAddress(lib, "AIMPPluginGetHeader");
#else
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto getHeader = (TAIMPPluginGetHeaderProc)dlsym(lib, "AIMPPluginGetHeader");
#endif
    CHECK(getHeader != nullptr);
    if (!getHeader) return 1;

    auto* core = new CoreImpl();
    core->profile = dir + "/";
#ifdef _WIN32
    if (argc > 3 && std::string(argv[3]) == "voicemeeter") {
        VoicemeeterRun(core, getHeader, dir);
        g_ui.reg.Clear();
        FreeLibrary(lib);
        printf(g_failures ? "%d check(s) failed\n" : "All checks passed\n", g_failures);
        return g_failures ? 1 : 0;
    }
#endif
    IAIMPPlugin* plugin = nullptr;
    CHECK(Succeeded(getHeader(&plugin)) && plugin);
    printf("Plugin: %s\n", N(plugin->InfoGet(AIMP_PLUGIN_INFO_NAME)).c_str());
    CHECK(Succeeded(plugin->Initialize(core)));
    CHECK(core->disp->hook != nullptr);
    CHECK(core->frame != nullptr);

    // Settings page: name, then build it with the mock UI service and check the layout
    IAIMPOptionsDialogFrame* frame = nullptr;
    CHECK(Succeeded(core->frame->QueryInterface(IID_IAIMPOptionsDialogFrame, (void**)&frame)) && frame);
    if (frame) {
        IAIMPString* name = nullptr;
        CHECK(Succeeded(frame->GetName(&name)) && name && N(tstr(name->GetData(), name->GetLength())) == "Prevent Resampling");
        // AIMP links the plugin list entry and the settings page by name
        if (name) CHECK(N(plugin->InfoGet(AIMP_PLUGIN_INFO_NAME)) == N(tstr(name->GetData(), name->GetLength())));
        CHECK(N(plugin->InfoGet(AIMP_PLUGIN_INFO_AUTHOR)) == "Fl4sh");
        IAIMPExternalSettingsDialog* dlg = nullptr;
        CHECK(Succeeded(plugin->QueryInterface(IID_IAIMPExternalSettingsDialog, (void**)&dlg)) && dlg);
        if (dlg) { dlg->Show((HWND)0); dlg->Release(); }
        if (name) name->Release();
        CHECK(frame->CreateFrame((HWND)0) != (HWND)0);
        CheckLayout(dir + "/layout.txt");
        BitPerfectNeedsPlugin();
        NewOnThePage();
        {
            // About tab: the changelog lists the installed version first and ends with the versions
            // before 2.0 (res/changelog-history.md)
            std::string log;
            for (auto& f : g_ui.reg.all) {
                auto i = f();
                for (auto& kv : *i.strs)
                    if (N(kv.second).find("(installed)") != std::string::npos) log = N(kv.second);
            }
            size_t installed = log.find("  (installed)"), v2 = log.find("Version 2.0.0"),
                   v1 = log.find("Version 1.0 (alpha, never released)");
            CHECK(installed != std::string::npos && v2 != std::string::npos && v1 != std::string::npos);
            CHECK(installed < v2 && v2 < v1);
            CHECK(log.find("AIMP AutoRate") != std::string::npos && log.find("Versions before 2.0") == std::string::npos);
            // every line fits the box (wrapped at 76 characters - list items and paragraphs alike)
            size_t longest = 0, cur = 0;
            for (unsigned char c : log) {
                if (c == '\n') { longest = (std::max)(longest, cur); cur = 0; }
                else if (c != '\r' && (c & 0xC0) != 0x80) cur++;  // count characters, not UTF-8 bytes
            }
            longest = (std::max)(longest, cur);
            if (longest > 76) printf("changelog: longest line %d characters\n", (int)longest);
            CHECK(longest <= 76);
        }
        AboutTab();
        LanguageAndSettingsFile(dir);
        frame->DestroyFrame();
        frame->Release();
    }

    // Fresh installation: the plugin must be disabled and must not touch anything
    StartTrack(core, 96000);
    WaitUntil([] { return false; }, 300);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    CHECK(IniGet("Statistics", "Tracks") == "0");
    CHECK(IniGet("PreventResampling", "Enabled") == "0");  // defaults written, plugin off
    CHECK(!IniGet("Updates", "RunVersion").empty() && g_ui.message.shown == 0);  // new installation: no update notice
    CHECK(IniGet("PreventResampling", "LegacyRestart") == "0" && IniGet("PreventResampling", "RestartAimp").empty());
    CHECK(core->menuItems.empty() && core->actions.empty());  // menus and hotkeys removed again
    CHECK(IniGet("PreventResampling", "MaxRate") == "0" && IniGet("PreventResampling", "SkipLossy") == "0" &&
          IniGet("PreventResampling", "MatchBitDepth") == "0");  // 2.7.0 defaults: nothing changes for an update
    // settings of older versions in AIMP's configuration are removed on the first start
    CHECK(core->config->deleted.size() == 2 && core->config->deleted[0] == "PreventResampling");
    CHECK(ReadFile(pwLog).empty());
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Plugin is disabled") != std::string::npos);
    CHECK(core->player->stops == 0 && core->shutdown->calls == 0);
    // AIMP's output at start-up comes from its settings (AIMP has not opened the output yet); not set here
    CHECK(ReadFile(dir + "/PreventResampling.log").find("AIMP output: not set in AIMP's settings (AIMP reports ") != std::string::npos);
    CHECK(ReadFile(dir + "/PreventResampling.log").find("AIMP output at start-up") == std::string::npos);
    // the settings file is in AIMP's profile folder, nothing in the old place (APPDATA / XDG_CONFIG_HOME)
    CHECK(ReadFile(dir + "/AIMP/PreventResampling.ini").empty());

    IniSet("Enabled", "1");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));

#ifdef _WIN32
    // Windows: flow without real hardware (Wine has no IPolicyConfig)
    for (int r : {96000, 44100, 48000}) {
        StartTrack(core, r);
        WaitUntil([] { return false; }, 300);
    }
    StreamNotSwitched(core, dir);
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(core->disp->hook == nullptr);
    CHECK(core->frame == nullptr);
    CHECK(IniGet("Statistics", "Tracks") == "4");
    plugin->Release();
    {
        std::string log = ReadFile(dir + "/PreventResampling.log");
        printf("---- PreventResampling.log ----\n%s----------------------\n", log.c_str());
        CHECK(log.find("Windows x") != std::string::npos && log.find("stopped") != std::string::npos);
        // The Windows audio API must be creatable (caught a typo in an IID)
        CHECK(log.find("MMDevice enumerator not available") == std::string::npos);
    }
    // ASIO mode without installed drivers
    IniSet("Mode", "3");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 88200);
    WaitUntil([] { return false; }, 300);
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(IniGet("Statistics", "Tracks") == "5");
    plugin->Release();
    CHECK(core->player->stops == core->player->plays);

    // AIMP 4 has no AIMP_PLAYER_PROPID_OUTPUT: the output is read from AIMP's settings
    IniSet("Mode", "0");
    core->player->aimp4 = true;
    core->config->v["AIMPSoundOut\\DeviceName"] = "DirectSound: Mock 4";
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("Track 96 kHz | DirectSound |") != std::string::npos; }));
    CHECK(ReadFile(dir + "/PreventResampling.log").find("AIMP output (from AIMP's settings): DirectSound: Mock 4") != std::string::npos);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    core->player->aimp4 = false;

    // Check the restart helper's INI editing directly (UTF-8 with BOM, value present)
    typedef void(CALLBACK * RestartProc)(HWND, HINSTANCE, LPWSTR, int);
    auto restartProc = (RestartProc)GetProcAddress(lib, "RestartAimpW");
    CHECK(restartProc != nullptr);
    if (restartProc) {
        FILE* ini = fopen((dir + "/test8.ini").c_str(), "wb");
        fputs("\xEF\xBB\xBF[System]\r\nX=1\r\n[AIMPSoundOut]\r\nDeviceName=ASIO: Test\r\nDeviceFreq=48000\r\nDeviceBitDepth=3\r\n", ini);
        fclose(ini);
        std::wstring args = L"0 44100 \"" + T(dir) + L"/test8.ini\" \"AIMPSoundOut\\DeviceFreq\" -";
        restartProc(nullptr, nullptr, &args[0], 0);
        std::string after = ReadFile(dir + "/test8.ini");
        CHECK(after.find("DeviceFreq=44100\r\n") != std::string::npos);
        CHECK(after.find("DeviceBitDepth=3") != std::string::npos && after.compare(0, 3, "\xEF\xBB\xBF") == 0);
        // Anything but a plain number is not written into AIMP.ini
        args = L"0 \"44100\r\n[X]\" \"" + T(dir) + L"/test8.ini\" \"AIMPSoundOut\\DeviceFreq\" -";
        restartProc(nullptr, nullptr, &args[0], 0);
        CHECK(ReadFile(dir + "/test8.ini") == after);
    }

    // AIMP outputs at a fixed 48 kHz (AIMP.ini in UTF-16), WASAPI exclusive, track at 96 kHz.
    {
        std::wstring w = L"\xFEFF[AIMPSoundOut]\r\nDeviceName=WASAPI Exclusive (Push): Windows Default\r\nDeviceFreq=48000\r\n";
        FILE* ini = fopen((dir + "/AIMP.ini").c_str(), "wb");
        fwrite(w.data(), sizeof(wchar_t), w.size(), ini);
        fclose(ini);
    }
    IniSet("Mode", "2");
    int stops0 = core->player->stops, plays0 = core->player->plays;
    //  a) 2.8 (default): AIMP's own rate changes without restart - invisibly, playback goes on
    g_opts.config = core->config;
    core->config->v["AIMPSoundOut\\DeviceFreq"] = "48000";
    core->player->info->file = "C:\\Music\\live.flac";
    auto logHas = [&](const std::string& t) { return ReadFile(dir + "/PreventResampling.log").find(t) != std::string::npos; };
    auto aimpRate = [&] { return core->config->v["AIMPSoundOut\\DeviceFreq"]; };
    IniSet("RestartAimp", "1");  // 2.7's experimental option does not switch the legacy method on
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    if (!CanCreateWindows()) {
        // Wine without a display (CI without Xvfb) creates no top-level windows, so there are no preferences:
        // the plugin gives up cleanly and AIMP keeps its rate
        printf("NOTE: no display - the window part of the test without restart needs one (xvfb-run)\n");
        StartTrack(core, 96000);
        CHECK(WaitUntil([&] { return logHas("AIMP's rate stays at 48 kHz: AIMP's preferences did not open"); }, 8000));
        CHECK(aimpRate() == "48000" && core->shutdown->calls == 0 && core->player->stops == stops0);
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
        g_noDisplay = true;
    } else {
        // the user closed AIMP's preferences on the Plugins page last time: the plugin's window opens on Sound
        // Output (Apply after the Plugins page would restart AIMP 6), the user's page is put back afterwards
        core->config->v["Options\\ActivePage"] = "17";
        StartTrack(core, 96000);
        CHECK(WaitUntil([&] { return logHas("AIMP: 48 kHz -> 96 kHz without restart"); }, 8000));
        CHECK(aimpRate() == "96000" && g_opts.applied == 1 && g_opts.opened == 1 && g_opts.restarts == 0);
        CHECK(g_opts.visible == 0 && !g_opts.form);  // never on screen, closed again
        CHECK(g_opts.activated <= 1);  // at most for the moment of Apply (the focus test is the live phase in real AIMP)
        CHECK(core->config->v["Options\\ActivePage"] == "17");
        CHECK(core->shutdown->calls == 0 && core->player->stops == stops0);
        //     same rate again: nothing to do
        StartTrack(core, 96000);
        WaitUntil([] { return false; }, 500);
        CHECK(g_opts.opened == 1);
        //     an AIMP that keeps its preferences (hidden)
        g_opts.keep = true;
        StartTrack(core, 44100);
        CHECK(WaitUntil([&] { return logHas("AIMP: 96 kHz -> 44.1 kHz without restart"); }, 8000));
        CHECK(aimpRate() == "44100" && g_opts.applied == 2 && g_opts.visible == 0 && g_opts.form && !IsWindowVisible(g_opts.form));
        StartTrack(core, 88200);  // the kept window again
        CHECK(WaitUntil([&] { return logHas("AIMP: 44.1 kHz -> 88.2 kHz without restart"); }, 8000));
        CHECK(aimpRate() == "88200" && g_opts.applied == 3 && g_opts.opened == 3 && g_opts.visible == 0);
        //     the kept window shows another page (the start page does not apply to it): nothing is pressed
        g_opts.Show(false);
        StartTrack(core, 96000);
        CHECK(WaitUntil([&] { return logHas("AIMP's rate stays at 88.2 kHz: AIMP's preferences opened on another page"); }, 8000));
        CHECK(aimpRate() == "88200" && g_opts.applied == 3 && g_opts.restarts == 0 && g_opts.visible == 0);
        CHECK(core->config->v["Options\\ActivePage"] == "17");
        //     the user has AIMP's preferences open: they are not touched until they are closed
        g_opts.keep = false;
        core->disp->Send(AIMP_MSG_CMD_OPTIONS, 0, nullptr);
        int shown = g_opts.visible;
        CHECK(shown > 0 && g_opts.form && IsWindowVisible(g_opts.form));
        StartTrack(core, 96000);
        CHECK(WaitUntil([&] { return logHas("AIMP's preferences are open - AIMP's rate follows when they are closed"); }));
        WaitUntil([] { return false; }, 300);
        CHECK(aimpRate() == "88200" && g_opts.applied == 3);
        SendMessageW(g_opts.form, WM_CLOSE, 0, 0);
        CHECK(!g_opts.form);
        core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION, 0, nullptr, nullptr);
        CHECK(WaitUntil([&] { return logHas("Output changed (AIMP's preferences closed)"); }));
        CHECK(WaitUntil([&] { return logHas("AIMP: 88.2 kHz -> 96 kHz without restart"); }, 8000));
        CHECK(g_opts.visible == shown && aimpRate() == "96000" && g_opts.restarts == 0);
        // the page the user closed them on (2.8.1: put back once the hidden window has closed in the background)
        CHECK(WaitUntil([&] { return core->config->v["Options\\ActivePage"] == "17"; }, 2000));
        //     a device that offers only a few rates (e.g. HDMI: 32, 44.1, 48 kHz): the best of them
        std::string out0 = core->player->output;
        core->player->output = "WASAPI Exclusive (Push): HDMI Monitor";
        g_opts.rates = {32000, 44100, 48000};
        core->config->v["AIMPSoundOut\\DeviceFreq"] = "44100";
        StartTrack(core, 192000);
        CHECK(WaitUntil([&] { return logHas("AIMP: 44.1 kHz -> 48 kHz without restart"); }, 8000));
        CHECK(aimpRate() == "48000");
        int opened = g_opts.opened;
        StartTrack(core, 96000);  // AIMP's list is known now: 48 kHz is the best it offers, no window needed
        WaitUntil([] { return false; }, 500);
        CHECK(g_opts.opened == opened && aimpRate() == "48000");
        StartTrack(core, 44100);
        CHECK(WaitUntil([&] { return logHas("AIMP: 48 kHz -> 44.1 kHz without restart"); }, 8000));
        CHECK(g_opts.visible == shown && g_opts.restarts == 0);
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
        CHECK(IniGet("Statistics", "AimpRateChanges") == "6");
        CHECK(core->shutdown->calls == 0 && core->player->stops == stops0);
        core->player->output = out0;
        g_opts.rates = MockOptions().rates;
    }
    //  b) legacy option (restart AIMP) -> stop playback, write the resume file, start the helper, close AIMP
    //     (do not keep playing). The helper (rundll32) waits for this process to end, writes 96000
    //     to AIMP.ini and starts MockHost.exe without arguments (prints usage only) - the CI script
    //     checks that afterwards.
    IniSet("LegacyRestart", "1");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return core->shutdown->calls == 1; }));
    CHECK(core->player->stops == stops0 + 1 && core->player->plays == plays0);
    CHECK(ReadFile(dir + "/PreventResampling.resume").find("96000") != std::string::npos);
    {
        // 5th line: AIMP's windows for the restarted AIMP ("<active>,<foreground>,<tick>|...")
        std::string r = ReadFile(dir + "/PreventResampling.resume");
        size_t pos = 0;
        for (int i = 0; i < 4 && pos < r.size(); i++) {
            size_t nl = r.find('\n', pos);
            pos = nl == std::string::npos ? r.size() : nl + 1;
        }
        CHECK(pos + 1 < r.size() && (r[pos] == '0' || r[pos] == '1') && r[pos + 1] == ',');
    }
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    // Closed by the plugin: the rates stay, the originals are handed to the next AIMP process
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Restart: kept the current rates") != std::string::npos);
    CHECK(IniGet("Session", "Saved") != "");
    //  c) the "restarted" AIMP takes the session over on its first track and removes it from the file
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("from before the restart") != std::string::npos; }));
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    CHECK(IniGet("Session", "Saved") == "");
    IniSet("Mode", "0");
    IniSet("LegacyRestart", "0");
    QuickSwitchRun(core, getHeader, dir);
    SettingsTakeOver(core, getHeader, dir);
    g_ui.reg.Clear();  // before unloading: the controls hold the plugin's event handlers
    FreeLibrary(lib);
#else
    // AIMP for Linux resamples every track to its own rate (AIMPSoundOut\DeviceFreq in AIMP.ini):
    // PipeWire must follow that rate, not the track's, or it resamples a second time
    auto setAimpRate = [&](int rate) {
        FILE* ini = fopen((dir + "/AIMP.ini").c_str(), "wb");
        fprintf(ini, "\xEF\xBB\xBF[System]\nVersion=6000\n[AIMPSoundOut]\nDeviceName=ALSA: default\nDeviceFreq=%d\n", rate);
        fclose(ini);
    };
    // 0) AIMP's rate unknown (no AIMP.ini yet) -> nothing is changed
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("AIMP's output rate unknown - nothing changed") != std::string::npos; }));
    CHECK(ReadFile(pwLog).find("clock.force-rate") == std::string::npos);
    // 1) AIMP plays at 96 kHz -> PipeWire must be forced to 96000, without stopping playback
    setAimpRate(96000);
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 96000") != std::string::npos; }));
    // 2) same rate again -> no further call
    size_t before = ReadFile(pwLog).size();
    StartTrack(core, 96000);
    WaitUntil([] { return false; }, 400);
    CHECK(ReadFile(pwLog).size() == before);
    // 3) 44.1 kHz
    setAimpRate(44100);
    StartTrack(core, 44100);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 44100") != std::string::npos; }));
    // 4) quick succession: only the last track counts in the end
    setAimpRate(88200);
    StartTrack(core, 48000);
    StartTrack(core, 88200);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 88200") != std::string::npos; }));
    CHECK(core->player->stops == 0 && core->player->plays == 0);
    // 4b) AIMP loads a track without playing it (e.g. restored paused at start-up): nothing may be
    //     switched or started until the user presses play
    core->player->state = AIMP_PLAYER_STATE_PAUSED;
    setAimpRate(176400);
    StartTrack(core, 176400);
    WaitUntil([] { return false; }, 400);
    CHECK(ReadFile(pwLog).find("clock.force-rate 176400") == std::string::npos);
    CHECK(core->player->plays == 0);
    core->player->state = AIMP_PLAYER_STATE_PLAYING;
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 176400") != std::string::npos; }));
    CHECK(core->player->plays == 0);

    // 5) AIMP outputs at a fixed 48 kHz, the track has 96 kHz: AIMP resamples to 48 kHz anyway, so
    //    PipeWire is set to 48 kHz (no second resampling), no restart on Linux, no interruption
    setAimpRate(48000);
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("AIMP rate: 48 kHz") != std::string::npos; }));
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 48000") != std::string::npos; }));
    CHECK(ReadFile(dir + "/PreventResampling.log").find("PipeWire follows AIMP") != std::string::npos);
    // 6) same rate again -> nothing to do
    StartTrack(core, 96000);
    WaitUntil([] { return false; }, 400);
    CHECK(core->player->stops == 0 && core->shutdown->calls == 0);
    // 7) internet radio: never switched
    StreamNotSwitched(core, dir);

    // Finalize: force-rate is reset, statistics are saved
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(ReadFile(pwLog).find("clock.force-rate 0") != std::string::npos);
    CHECK(core->disp->hook == nullptr);
    CHECK(core->frame == nullptr);
    CHECK(IniGet("Statistics", "Tracks") == "10");
    CHECK(IniGet("Statistics", "Rates").find("96000=5") != std::string::npos);
    plugin->Release();

    std::string log = ReadFile(dir + "/PreventResampling.log");
    printf("---- PreventResampling.log ----\n%s----------------------\n", log.c_str());
    printf("---- pw-metadata calls ----\n%s-----------------------------\n", ReadFile(pwLog).c_str());
    CHECK(log.find("started") != std::string::npos && log.find("stopped") != std::string::npos);

    // Second run: statistics are loaded from the configuration; playback after a restart by the
    // plugin (resume file) continues after AIMP_MSG_EVENT_LOADED
    {
        FILE* rf = fopen((dir + "/PreventResampling.resume").c_str(), "wb");
        fprintf(rf, "/music/resume.flac\n12500\n96000\n%lld\n", (long long)time(nullptr));
        fclose(rf);
    }
    // ... and an update is found on start-up: downloaded, SHA-256 checked, opened in AIMP. The repository
    // still has its old name here: the new name is not found, the release comes from the old one
    const std::string pkg = "PK-fake-aimppack-content";
    const std::string pkgUrl = "https://github.com/RainBowFl4sh/AIMP-No-Resmapling/releases/download/v9.9.9/PreventResampling-9.9.9.aimppack";
    core->http->pages["https://api.github.com/repos/RainBowFl4sh/AIMP-No-Resmapling/releases/latest"] =
        "{\"tag_name\": \"v9.9.9\", \"body\": \"Fixes \\u00e4\\r\\n- one\", \"assets\": [{\"name\": "
        "\"PreventResampling-9.9.9.aimppack\", \"browser_download_url\": \"" + pkgUrl + "\", "
        "\"digest\": \"sha256:" + ar::Sha256::Of(pkg) + "\"}]}";
    core->http->pages[pkgUrl] = pkg;
    IniSet("UpdateInterval", "0");
    IniSet("UpdateAutoInstall", "1");
    int playsBefore = core->player->plays;
    core->player->seekFailures = 1;  // seek right after Play fails, it is repeated at the stream start
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling-9.9.9.aimppack") == pkg; }));
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("SHA-256 OK") != std::string::npos; }));
    CHECK(IniGet("Updates", "AutoOpened") == "9.9.9" && IniGet("Updates", "LatestVersion") == "9.9.9");
    CHECK(IniGet("Updates", "LastCheckFailed") == "0");
    CHECK(core->http->requested.size() == 3);
    CHECK(core->http->requested[0] == "https://api.github.com/repos/RainBowFl4sh/AIMP-No-Resampling/releases/latest");
    // AIMP installs the package (the plugin file on disk changes) -> the plugin restarts AIMP to load it
    // and writes the resume file for the track that is playing
    {
        struct stat st;
        CHECK(stat(argv[1], &st) == 0);
        struct timeval tv[2] = {{(time_t)(st.st_mtime + 60), 0}, {(time_t)(st.st_mtime + 60), 0}};
        CHECK(utimes(argv[1], tv) == 0);
        CHECK(WaitUntil([&] { return core->shutdown->restarts == 1; }, 8000));
        CHECK(ReadFile(dir + "/PreventResampling.resume").find("track") != std::string::npos);
        CHECK(ReadFile(dir + "/PreventResampling.log").find("Update installed - restarting AIMP") != std::string::npos);
        remove((dir + "/PreventResampling.resume").c_str());
    }
    CHECK(core->player->plays == playsBefore + 1);
    CHECK(core->player->seekPos < 0);  // not yet: the track is still opening
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_STREAM_START, 0, nullptr, nullptr);
    CHECK(core->player->seekPos > 12.4 && core->player->seekPos < 12.6);  // continued at 12.5 s
    CHECK(ReadFile(dir + "/PreventResampling.resume").empty());  // the file is removed after reading
    setAimpRate(192000);
    StartTrack(core, 192000);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 192000") != std::string::npos; }));
    setAimpRate(176400);
    // A track that AIMP restored at start-up plays without any stream-start event: pressing play
    // must still switch to its rate. Pausing and resuming the same track again does nothing.
    auto count = [&](const std::string& what) {
        std::string t = ReadFile(pwLog);
        size_t n = 0;
        for (size_t p = t.find(what); p != std::string::npos; p = t.find(what, p + 1)) n++;
        return n;
    };
    size_t before176 = count("clock.force-rate 176400");
    core->player->info->rate = 176400;  // another file, loaded without AIMP_MSG_EVENT_STREAM_START
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    CHECK(WaitUntil([&] { return count("clock.force-rate 176400") == before176 + 1; }));
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PAUSED, nullptr, nullptr);
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    WaitUntil([] { return false; }, 400);
    CHECK(count("clock.force-rate 176400") == before176 + 1);
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(IniGet("Statistics", "Tracks") == "13");
    plugin->Release();

    // Start after an update (the last start was an older version): the user is told once, after loading
    IniSetIn("Updates", "RunVersion", "2.5.5");
    IniSetIn("Restart", "ReopenSettings", "1");  // "Restart AIMP now" was pressed on the Output tab
    int pagesShown = g_options.shown;
    size_t ctlBefore = g_ui.reg.all.size();
    g_options.createOnShow = true;
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    CHECK(g_ui.message.shown == 0);  // not before AIMP has loaded
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
    // the settings open again once AIMP has started, on the same tab
    CHECK(WaitUntil([&] { return g_options.shown == pagesShown + 1; }));
    CHECK(IniGet("Restart", "ReopenSettings").empty());
    {
        IAIMPOptionsDialogFrame* page = nullptr;
        CHECK(core->frame && Succeeded(core->frame->QueryInterface(IID_IAIMPOptionsDialogFrame, (void**)&page)) && page);
        if (page) {
            void* tabs = nullptr;
            std::vector<void*> sheets;
            IUnknown* active = nullptr;
            for (size_t k = ctlBefore; k < g_ui.reg.all.size(); k++) {
                auto i = g_ui.reg.all[k]();
                if (N(i.kind) == "Tabs") { tabs = i.self; active = i.objs->count(AIMPUI_PAGECONTROL_PROPID_ACTIVE) ? i.objs->at(AIMPUI_PAGECONTROL_PROPID_ACTIVE) : nullptr; }
                if (i.kind == mockui::Service::K("S")) sheets.push_back(i.self);
            }
            CHECK(tabs && sheets.size() >= 2 && active && static_cast<void*>(static_cast<IAIMPUIControl*>(static_cast<IAIMPUITabSheet*>(active))) == sheets[1]);
            page->DestroyFrame();
            page->Release();
        }
    }
    g_options.createOnShow = false;
    CHECK(WaitUntil([] { return g_ui.message.shown == 1; }));
    CHECK(N(g_ui.message.caption) == "Prevent Resampling");
    CHECK(N(g_ui.message.text).find("was updated to version") != std::string::npos &&
          N(g_ui.message.text).find("About") != std::string::npos);
    CHECK(IniGet("Updates", "RunVersion") != "2.5.5");
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Updated from 2.5.5 to") != std::string::npos);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();

    // Further runs: a failed update check (GitHub not reachable) is marked as failed (not shown as
    // "latest version") and repeated at the next start after an hour, although the weekly interval
    // has not passed - not at a start right after it (e.g. a restart by the plugin)
    core->http->pages.clear();
    for (int run = 0; run < 3; run++) {
        IniSet("UpdateInterval", run == 0 ? "0" : "2");
        if (run == 2) IniSetIn("Updates", "LastCheck", std::to_string((long long)time(nullptr) - 3700));
        size_t requests = core->http->requested.size();
        plugin = nullptr;
        getHeader(&plugin);
        CHECK(Succeeded(plugin->Initialize(core)));
        core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
        if (run == 1) {
            WaitUntil([] { return false; }, 300);
            CHECK(core->http->requested.size() == requests);
        } else {
            CHECK(WaitUntil([&] { return core->http->requested.size() == requests + 2 && IniGet("Updates", "LastCheckFailed") == "1"; }));  // new and old name
        }
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
    }
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Update: Update check failed") != std::string::npos);
    CHECK(g_ui.message.shown == 1);  // the update notice only once per version

    // A release whose package is not in this repository's releases, or whose name is not a plain file
    // name, is shown but never downloaded
    core->http->pages["https://api.github.com/repos/RainBowFl4sh/AIMP-No-Resampling/releases/latest"] =
        "{\"tag_name\": \"v9.9.10\", \"body\": \"\", \"assets\": ["
        "{\"name\": \"PreventResampling-9.9.10.aimppack\", \"browser_download_url\": \"https://example.invalid/pkg\", \"digest\": \"sha256:" + ar::Sha256::Of(pkg) + "\"},"
        "{\"name\": \"..\\\\x.aimppack\", \"browser_download_url\": \"https://github.com/RainBowFl4sh/AIMP-No-Resampling/releases/download/v9.9.10/x.aimppack\", \"digest\": \"sha256:" + ar::Sha256::Of(pkg) + "\"}]}";
    IniSet("UpdateInterval", "0");
    {
        size_t requests = core->http->requested.size();
        plugin = nullptr;
        getHeader(&plugin);
        CHECK(Succeeded(plugin->Initialize(core)));
        core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
        CHECK(WaitUntil([&] { return IniGet("Updates", "LatestVersion") == "9.9.10"; }));
        WaitUntil([] { return false; }, 300);
        CHECK(core->http->requested.size() == requests + 1);  // only the release information (found under the new name)
        CHECK(IniGet("Updates", "AutoOpened") == "9.9.9");
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
    }

    QuickSwitchRun(core, getHeader, dir);
    SettingsTakeOver(core, getHeader, dir);
    g_ui.reg.Clear();
    dlclose(lib);
#endif
    printf(g_failures ? "%d check(s) failed\n" : "All checks passed\n", g_failures);
#ifdef _WIN32
    if (g_noDisplay) printf("(without a display: the window part of the test without restart was not run)\n");
#endif
    return g_failures ? 1 : 0;
}
