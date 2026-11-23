// Windows audio devices: enumeration and the "default format" (shared-mode rate) via IPolicyConfig.
#pragma once
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <atomic>

#include "../Common.h"

namespace ar {
namespace win {

// Undocumented COM interface (Windows 7 to 11), also used by SoundSwitch, AudioDeviceCmdlets
// and others. The order of the methods must NOT be changed.
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static const CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID IID_IPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

// Own copies of the GUIDs/keys so no import libraries are needed (same for MinGW and MSVC)
static const CLSID CLSID_MMDeviceEnumerator_ = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID IID_IMMDeviceEnumerator_ = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID IID_IAudioClient_ = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const PROPERTYKEY KEY_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// PKEY_AudioEngine_DeviceFormat = "Default Format" in the Windows sound settings
static const PROPERTYKEY KEY_DeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

template <typename T>
class Com {
    T* p_ = nullptr;

public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p_) p_->Release(); }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void** Out() { if (p_) { p_->Release(); p_ = nullptr; } return (void**)&p_; }
    T** OutT() { return (T**)Out(); }
};

struct Endpoint {
    std::wstring id, name;
    // Voicemeeter/VB-Audio devices are recognised by name (the vendor is always part of it)
    bool IsVB() const { return ContainsI(name, L"voicemeeter") || ContainsI(name, L"vb-audio"); }
};

inline std::wstring Hex(HRESULT hr);

inline bool Enumerator(Com<IMMDeviceEnumerator>& e) {
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator_, e.Out());
    if (FAILED(hr)) Log(L"MMDevice enumerator not available (" + Hex(hr) + L")");
    return SUCCEEDED(hr);
}

inline bool ToEndpoint(IMMDevice* d, Endpoint& out) {
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id))) return false;
    out.id = id;
    CoTaskMemFree(id);
    Com<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, ps.OutT()))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(KEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) out.name = v.pwszVal;
    PropVariantClear(&v);
    return true;
}

inline bool DefaultRender(Endpoint& out) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return false;
    Com<IMMDevice> d;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, d.OutT()))) return false;
    return ToEndpoint(d.Get(), out);
}

inline std::vector<Endpoint> ListRender() {
    std::vector<Endpoint> r;
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return r;
    Com<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.OutT()))) return r;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        Com<IMMDevice> d;
        Endpoint e;
        if (SUCCEEDED(col->Item(i, d.OutT())) && ToEndpoint(d.Get(), e)) r.push_back(e);
    }
    return r;
}

// Voicemeeter names an A1 device e.g. "FiiO ASIO Driver", Windows calls it "Speakers (FiiO KA11)".
// Matching uses distinctive words (vendor/model); generic words are ignored.
inline bool NamesMatch(const std::wstring& busName, const std::wstring& endpointName) {
    if (busName.empty() || endpointName.empty()) return false;
    if (ContainsI(endpointName, busName) || ContainsI(busName, endpointName)) return true;
    static const wchar_t* generic[] = {L"asio", L"driver", L"usb", L"audio", L"device", L"wdm", L"mme", L"ks",
                                       L"output", L"out", L"speakers", L"speaker", L"lautsprecher", L"headphones",
                                       L"kopfhoerer", L"kopfhörer", L"digital", L"high", L"definition", L"the",
                                       L"and", L"for", L"sound", L"card", L"control", L"panel", L"v2", L"2.0"};
    std::vector<std::wstring> tokens;
    std::wstring cur;
    for (wchar_t c : busName + L" ") {
        if (iswalnum(c)) cur += (wchar_t)towlower(c);
        else {
            bool skip = cur.size() < 3;
            for (auto* g : generic) if (cur == g) skip = true;
            if (!skip) tokens.push_back(cur);
            cur.clear();
        }
    }
    if (tokens.empty()) return false;
    for (auto& t : tokens) if (!ContainsI(endpointName, t)) return false;
    return true;
}

inline bool FindByName(const std::vector<Endpoint>& all, const std::wstring& part, Endpoint& out) {
    for (auto& e : all)  // exact name first
        if (EqualsI(e.name, part)) { out = e; return true; }
    for (auto& e : all)
        if (ContainsI(e.name, part)) { out = e; return true; }
    return false;
}

// Finds the Windows device whose name appears in AIMP's output name
// (e.g. "WASAPI: Speakers (Realtek(R) Audio)"). The longest match wins.
inline bool FindInText(const std::vector<Endpoint>& all, const std::wstring& text, Endpoint& out) {
    size_t best = 0;
    for (auto& e : all)
        if (!e.name.empty() && e.name.size() > best && ContainsI(text, e.name)) { out = e; best = e.name.size(); }
    return best > 0;
}

// Only such formats are handed to Windows: cbSize must not claim more bytes than the structure holds
inline bool PlausibleFormat(const WAVEFORMATEXTENSIBLE& f) {
    return f.Format.nChannels > 0 && f.Format.nSamplesPerSec > 0 &&
           f.Format.cbSize <= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
}

inline bool GetDeviceFormat(const std::wstring& id, WAVEFORMATEXTENSIBLE& fmt) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return false;
    Com<IMMDevice> d;
    if (FAILED(en->GetDevice(id.c_str(), d.OutT()))) return false;
    Com<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, ps.OutT()))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(ps->GetValue(KEY_DeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        ZeroMemory(&fmt, sizeof(fmt));
        memcpy(&fmt, v.blob.pBlobData, (std::min)((size_t)v.blob.cbSize, sizeof(fmt)));
        ok = PlausibleFormat(fmt);
    }
    PropVariantClear(&v);
    return ok;
}

inline WAVEFORMATEXTENSIBLE WithRate(WAVEFORMATEXTENSIBLE f, uint32_t hz) {
    f.Format.nSamplesPerSec = hz;
    f.Format.nAvgBytesPerSec = hz * f.Format.nBlockAlign;
    return f;
}

static const GUID SUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID SUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline bool IsExtensible(const WAVEFORMATEXTENSIBLE& f) {
    return f.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && f.Format.cbSize >= 22;
}
// Bits that carry the signal (24 in a 32-bit container = 24)
inline uint32_t ValidBits(const WAVEFORMATEXTENSIBLE& f) {
    if (IsExtensible(f) && f.Samples.wValidBitsPerSample) return f.Samples.wValidBitsPerSample;
    return f.Format.wBitsPerSample;
}
inline bool IsFloat(const WAVEFORMATEXTENSIBLE& f) {
    return f.Format.wFormatTag == 3 /* WAVE_FORMAT_IEEE_FLOAT */ ||
           (IsExtensible(f) && memcmp(&f.SubFormat, &SUBTYPE_FLOAT, sizeof(GUID)) == 0);
}
// "24 bit" / "32 bit float"
inline std::wstring BitsText(const WAVEFORMATEXTENSIBLE& f) {
    return Num(ValidBits(f)) + (IsFloat(f) ? L" bit float" : L" bit");
}

// The same format with another bit depth as integer PCM (container: bytes per sample x 8, valid: bits used)
inline WAVEFORMATEXTENSIBLE WithBits(WAVEFORMATEXTENSIBLE f, WORD container, WORD valid) {
    WORD ch = f.Format.nChannels;
    DWORD mask = IsExtensible(f) ? f.dwChannelMask : (ch == 1 ? 4u : ch == 2 ? 3u : 0u);
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.cbSize = 22;
    f.Format.wBitsPerSample = container;
    f.Samples.wValidBitsPerSample = valid;
    f.dwChannelMask = mask;
    f.SubFormat = SUBTYPE_PCM;
    f.Format.nBlockAlign = (WORD)(ch * container / 8);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    return f;
}

// Same rate, channels and sample format (what Windows reports may differ in unimportant fields)
inline bool SameFormat(const WAVEFORMATEXTENSIBLE& a, const WAVEFORMATEXTENSIBLE& b) {
    return a.Format.nSamplesPerSec == b.Format.nSamplesPerSec && a.Format.nChannels == b.Format.nChannels &&
           a.Format.wBitsPerSample == b.Format.wBitsPerSample && ValidBits(a) == ValidBits(b) && IsFloat(a) == IsFloat(b);
}

// Asks the driver whether it supports this format in exclusive mode (= real hardware capability).
// 1 = yes, 0 = no, -1 = unknown (e.g. the device is held exclusively by Voicemeeter).
inline int SupportsExclusive(const std::wstring& id, WAVEFORMATEXTENSIBLE f) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return -1;
    Com<IMMDevice> d;
    if (FAILED(en->GetDevice(id.c_str(), d.OutT()))) return -1;
    Com<IAudioClient> ac;
    if (FAILED(d->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, ac.Out()))) return -1;
    HRESULT hr = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&f, nullptr);
    if (hr == S_OK) return 1;
    if (hr == S_FALSE || hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return 0;
    return -1;
}

// Vista variant of the interface (without ResetDeviceFormat) as a fallback
struct IPolicyConfigVista : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};
static const CLSID CLSID_PolicyConfigVistaClient = {0x294935CE, 0xF637, 0x4E7C, {0xA4, 0x1B, 0xAB, 0x25, 0x54, 0x60, 0xB8, 0x62}};
static const IID IID_IPolicyConfigVista = {0x568b9108, 0x44bf, 0x40b4, {0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20}};
static const GUID SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline std::wstring Hex(HRESULT hr) {
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return buf;
}

// Mix format of the Windows mixer (always 32-bit float) matching the new device format
inline WAVEFORMATEXTENSIBLE MixFormatFor(const WAVEFORMATEXTENSIBLE& dev, const WAVEFORMATEX* current) {
    WAVEFORMATEXTENSIBLE m;
    ZeroMemory(&m, sizeof(m));
    if (current && current->wFormatTag == WAVE_FORMAT_EXTENSIBLE && current->cbSize >= 22)
        memcpy(&m, current, sizeof(m));
    else {
        m.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        m.Format.cbSize = 22;
        m.Format.nChannels = dev.Format.nChannels;
        m.dwChannelMask = dev.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? dev.dwChannelMask
                                                                          : (dev.Format.nChannels == 1 ? 4u : 3u);
        m.SubFormat = SUBTYPE_IEEE_FLOAT;
        m.Format.wBitsPerSample = 32;
        m.Samples.wValidBitsPerSample = 32;
    }
    m.Format.nSamplesPerSec = dev.Format.nSamplesPerSec;
    m.Format.nBlockAlign = (WORD)(m.Format.nChannels * m.Format.wBitsPerSample / 8);
    m.Format.nAvgBytesPerSec = m.Format.nSamplesPerSec * m.Format.nBlockAlign;
    return m;
}

// Sets a device's "default format" (shared-mode rate) and verifies the result.
// callMs (optional): how long Windows took for the call itself (the rest is waiting until it reports the format)
inline bool SetDeviceFormat(const std::wstring& id, WAVEFORMATEXTENSIBLE f, int64_t* callMs = nullptr) {
    int64_t callStart = NowMs();
    HRESULT hr = E_FAIL;
    Com<IPolicyConfig> pc;
    HRESULT co = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, pc.Out());
    if (SUCCEEDED(co)) {
        WAVEFORMATEX* cur = nullptr;
        pc->GetMixFormat(id.c_str(), &cur);
        WAVEFORMATEXTENSIBLE mix = MixFormatFor(f, cur);
        if (cur) CoTaskMemFree(cur);
        hr = pc->SetDeviceFormat(id.c_str(), (WAVEFORMATEX*)&f, (WAVEFORMATEX*)&mix);
        if (FAILED(hr)) Log(L"  IPolicyConfig::SetDeviceFormat: " + Hex(hr));
    } else {
        Log(L"  IPolicyConfig not available (" + Hex(co) + L")");
    }
    if (FAILED(hr)) {
        Com<IPolicyConfigVista> pv;
        if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigVistaClient, nullptr, CLSCTX_ALL, IID_IPolicyConfigVista, pv.Out()))) {
            WAVEFORMATEX* cur = nullptr;
            pv->GetMixFormat(id.c_str(), &cur);
            WAVEFORMATEXTENSIBLE mix = MixFormatFor(f, cur);
            if (cur) CoTaskMemFree(cur);
            hr = pv->SetDeviceFormat(id.c_str(), (WAVEFORMATEX*)&f, (WAVEFORMATEX*)&mix);
            if (FAILED(hr)) Log(L"  IPolicyConfigVista::SetDeviceFormat: " + Hex(hr));
        }
    }
    if (callMs) *callMs = NowMs() - callStart;
    // Did it really change? (Windows sometimes reports success without changing anything)
    WAVEFORMATEXTENSIBLE check;
    for (int i = 0; i < 20; i++) {
        if (GetDeviceFormat(id, check) && check.Format.nSamplesPerSec == f.Format.nSamplesPerSec &&
            check.Format.wBitsPerSample == f.Format.wBitsPerSample && ValidBits(check) == ValidBits(f))
            return true;
        SleepMs(25);
    }
    if (SUCCEEDED(hr)) Log(L"  Windows accepted the call but did not change the format");
    return false;
}

// ---- Audio chain (U2): what else Windows does with the signal of a device

static const IID IID_IAudioEndpointVolume_ = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
// PKEY_AudioEndpoint_Disable_SysFx: 1 = "audio enhancements" switched off for the device
static const PROPERTYKEY KEY_DisableSysFx = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 5};

struct DeviceState {
    bool found = false;
    WAVEFORMATEXTENSIBLE format{};  // Windows default format (shared mode)
    bool formatKnown = false;
    int volume = -1;                // Windows volume of the device in %, -1 = unknown
    bool muted = false;
    int enhancements = -1;          // 1 = on, 0 = off, -1 = unknown
};

inline DeviceState ReadDeviceState(const std::wstring& id) {
    DeviceState st;
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> d;
    if (id.empty() || !Enumerator(en) || FAILED(en->GetDevice(id.c_str(), d.OutT()))) return st;
    st.found = true;
    st.formatKnown = GetDeviceFormat(id, st.format);
    Com<IAudioEndpointVolume> vol;
    if (SUCCEEDED(d->Activate(IID_IAudioEndpointVolume_, CLSCTX_ALL, nullptr, vol.Out())) && vol) {
        float level = 0;
        BOOL mute = FALSE;
        if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&level))) st.volume = (int)(level * 100.0f + 0.5f);
        if (SUCCEEDED(vol->GetMute(&mute))) st.muted = mute != FALSE;
    }
    Com<IPropertyStore> ps;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, ps.OutT()))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(KEY_DisableSysFx, &v)) && (v.vt == VT_UI4 || v.vt == VT_I4)) st.enhancements = v.ulVal ? 0 : 1;
        PropVariantClear(&v);
    }
    return st;
}

// ---- Changes of the outputs (B5): Windows reports a new default device, a device that appears or
// disappears, or a changed default format. The calls come on a thread of Windows' audio service; only the
// time and the kind are noted here, the plugin looks at them once a second on AIMP's main thread.
static const IID IID_IMMNotificationClient_ = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};

class DeviceWatcher final : public IMMNotificationClient {
    Com<IMMDeviceEnumerator> en_;
    bool registered_ = false;
    std::atomic<int64_t>* at_;  // NowMs() of the last change (owned by the engine, read on the main thread)
    std::atomic<int>* what_;    // 1 = default device, 2 = default format, 3 = device added / removed

    void Mark(int kind) {
        *what_ = kind;
        *at_ = NowMs();
    }

public:
    DeviceWatcher(std::atomic<int64_t>* at, std::atomic<int>* what) : at_(at), what_(what) {}

    // Registered and unregistered on the worker (COM); false = not available (then only AIMP's own output and
    // Voicemeeter are watched)
    bool Start() {
        if (registered_ || !Enumerator(en_)) return registered_;
        registered_ = SUCCEEDED(en_->RegisterEndpointNotificationCallback(this));
        return registered_;
    }
    void Stop() {
        if (registered_ && en_) en_->UnregisterEndpointNotificationCallback(this);
        registered_ = false;
        en_.Out();  // released on the thread that created it
    }

    // A stack object of the worker; Windows' references only keep it while it is registered
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IMMNotificationClient_)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Mark(3); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) Mark(1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        if (IsEqualGUID(key.fmtid, KEY_DeviceFormat.fmtid) && key.pid == KEY_DeviceFormat.pid) Mark(2);
        return S_OK;
    }
};

}  // namespace win
}  // namespace ar
