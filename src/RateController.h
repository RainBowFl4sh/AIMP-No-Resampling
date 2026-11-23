// Decides which rate has to be set for the current output and sets it.
// Runs on the worker thread only (on Windows initialised as COM STA).
#pragma once
#include <ctime>
#include <map>
#include <set>

#include "Config.h"
#include "Hardware.h"
#include "Stats.h"

#ifdef _WIN32
#include "win/Asio.h"
#include "win/Voicemeeter.h"
#include "win/Wasapi.h"
#else
#include "linux/PipeWire.h"
#endif

namespace ar {

struct TrackInfo {
    uint32_t rate = 0, bits = 0, channels = 0;
    tstring codec, aimpOutput;
    bool lossy = false;  // MP3, AAC ... (no bit depth of its own)
};

struct Decision {
    OutputMode mode = OutputMode::Auto;
    uint32_t rate = 0;          // chosen target rate; 0 = none found
    bool apply = false;         // something has to be changed
    bool stopPlayback = false;  // playback has to be stopped for that
    uint32_t currentRate = 0;   // rate the output runs at right now (statistics)
    tstring chain, info;
    tstring endName, endId;     // the device at the end of the chain (what is heard), Windows id if known
    tstring aimpName, aimpId;   // Windows: the device AIMP plays into (a Voicemeeter input in a Voicemeeter chain)
#ifdef _WIN32
    std::vector<win::Endpoint> targets;  // order: extra devices, Voicemeeter outputs, AIMP's device LAST
    std::vector<std::pair<WORD, WORD>> bits;  // per target: new bit depth (container, valid); 0 = keep
    bool vm = false, vmEngine = false;
    bool vmNotRunning = false;  // the output goes to Voicemeeter, but Voicemeeter is not running (yet)
    bool asioDirect = false;
    win::AsioDriver asio;
#endif
};

inline const TChar* ModeName(OutputMode m) {
    switch (m) {
        case OutputMode::Auto: return AR_T("Automatic");
        case OutputMode::Shared: return AR_T("WASAPI shared");
        case OutputMode::Exclusive: return AR_T("WASAPI exclusive");
        case OutputMode::Asio: return AR_T("ASIO");
        case OutputMode::PipeWire: return AR_T("PipeWire");
        case OutputMode::Direct: return AR_T("ALSA direct");
        case OutputMode::DirectSound: return AR_T("DirectSound");
    }
    return AR_T("?");
}

// Output type for the setting (Automatic = from AIMP's output name)
inline OutputMode ResolveMode(OutputMode setting, const tstring& aimpOutput) {
    if (setting != OutputMode::Auto) return setting;
#ifdef _WIN32
    if (ContainsI(aimpOutput, AR_T("asio"))) return OutputMode::Asio;
    if (ContainsI(aimpOutput, AR_T("exclusive")) || ContainsI(aimpOutput, AR_T("exklusiv"))) return OutputMode::Exclusive;
    if (ContainsI(aimpOutput, AR_T("directsound"))) return OutputMode::DirectSound;
    return OutputMode::Shared;
#else
    (void)aimpOutput;
    return lnx::PipeWire::Available() ? OutputMode::PipeWire : OutputMode::Direct;
#endif
}

class RateController {
    Config cfg_;
    Timing timing_;
#ifdef _WIN32
    std::map<std::wstring, WAVEFORMATEXTENSIBLE> original_;  // for "restore when AIMP closes"
    uint32_t vmOriginalRate_ = 0;
    bool vmUsed_ = false;  // a Voicemeeter chain was switched in this session
    uint32_t vmSet_ = 0;  // last Option.sr set by the plugin (Voicemeeter sometimes returns stale values)
    uint32_t asioRateSet_ = 0;
    bool asioBusy_ = false;
    std::set<std::wstring> busyLogged_;  // devices held by Voicemeeter, logged once each
#else
    lnx::PipeWire pw_;
#endif

    // Same rate first, then integer multiples of the same family (44.1 -> 88.2 -> 176.4 etc.), never above the
    // highest rate of the settings (the track's rate is already limited to it, see Engine::Handle)
    std::vector<uint32_t> Candidates(uint32_t hz) const {
        std::vector<uint32_t> c{hz};
        if (!cfg_.allowMultiples) return c;
        uint32_t base = (hz % 44100 == 0) ? 44100 : (hz % 48000 == 0) ? 48000 : 0;
        if (base)
            for (uint32_t m = hz / base * 2; m <= 8 && (!cfg_.maxRate || base * m <= cfg_.maxRate); m *= 2) c.push_back(base * m);
        return c;
    }

public:
    // How long the steps of the last Apply took (ms), for the timing line in the log
    struct Times {
        int64_t devices = 0;  // Windows formats of the devices (Linux: PipeWire)
        int64_t asio = 0;     // ASIO driver rate
        int64_t vm = 0;       // Voicemeeter: rate set, engine restarted and running again
        int64_t settle = 0;   // fixed pause afterwards
    };
    Times last;

    void SetConfig(const Config& c, const Timing& t) {
        if (c.asioDriver != cfg_.asioDriver) ResetAsio();
        cfg_ = c;
        timing_ = t;
    }

    OutputMode Resolve(const tstring& aimpOutput) const { return ResolveMode(cfg_.mode, aimpOutput); }

#ifdef _WIN32
    void ResetAsio() { asioBusy_ = false; asioRateSet_ = 0; }

    // B5: Voicemeeter's engine rate may have been changed in Voicemeeter meanwhile. The rate the plugin set is
    // normally trusted (Option.sr can report a stale value right after a switch); seconds later, when an
    // output change is checked, Voicemeeter's own value counts.
    void ResyncVoicemeeter() {
        if (!vmSet_) return;
        uint32_t now = win::Voicemeeter::Get().PreferredRate();
        if (now) vmSet_ = now;
    }

    // ---------------------------------------------------------------- Windows
    Decision Prepare(const TrackInfo& t) {
        Decision d;
        d.mode = Resolve(t.aimpOutput);
        bool sharedLike = d.mode == OutputMode::Shared || d.mode == OutputMode::DirectSound;
        auto all = win::ListRender();
        auto add = [&](const win::Endpoint& e) {
            for (auto& x : d.targets) if (x.id == e.id) return;
            d.targets.push_back(e);
        };
        for (auto& n : Split(cfg_.extraDevices, L';')) {
            win::Endpoint e;
            if (win::FindByName(all, n, e)) add(e);
            else Log(L"Extra device '" + n + L"' not found");
        }

        // Device AIMP plays into
        win::Endpoint primary;
        bool havePrimary = false;
        if (!cfg_.windowsDevice.empty()) havePrimary = win::FindByName(all, cfg_.windowsDevice, primary);
        else if (d.mode != OutputMode::Asio) havePrimary = win::FindInText(all, t.aimpOutput, primary) || win::DefaultRender(primary);

        bool vbAsio = false;
        if (d.mode == OutputMode::Asio) {
            if (win::FindAsioDriver(cfg_.asioDriver, t.aimpOutput, d.asio)) {
                vbAsio = ContainsI(d.asio.name, L"voicemeeter") || ContainsI(d.asio.name, L"vb-audio");
                d.asioDirect = !vbAsio && cfg_.asioForce && !asioBusy_;
                d.chain = L"ASIO: " + d.asio.name;
            } else {
                d.chain = L"ASIO driver not found";
            }
        }

        // Voicemeeter: hardware outputs A1 (or A1..A5) are mapped to Windows devices by name
        auto& vm = win::Voicemeeter::Get();
        bool toVm = cfg_.vmEnabled && ((havePrimary && primary.IsVB()) || vbAsio);
        if (toVm && !vm.Running()) d.vmNotRunning = vm.Installed();
        else if (toVm) {
            d.vm = true;
            d.vmEngine = cfg_.vmEngineRate;
            for (auto& bus : vm.BusDevices(cfg_.vmAllBuses)) {
                if (!cfg_.BusSwitched(bus.first)) continue;  // A2-A5 ticked off on the Voicemeeter tab
                for (auto& e : all)
                    if (!e.IsVB() && win::NamesMatch(bus.second, e.name)) add(e);
            }
        }
        // Shared/DirectSound: AIMP's device itself is switched (exclusive/ASIO: AIMP opens it itself)
        if (havePrimary && (sharedLike || (d.vm && d.mode == OutputMode::Exclusive))) add(primary);
        // AIMP does not play into Voicemeeter, but Voicemeeter runs and holds one of these devices on a hardware
        // output (A1-A5): changing its Windows format would make AIMP report "Device is busy" - leave it alone
        if (!d.vm && win::Voicemeeter::ProcessRunning() && vm.Running()) {
            auto held = vm.BusDevices(true);
            for (size_t i = 0; i < d.targets.size();) {
                bool busy = false;
                int bus = 0;
                if (!d.targets[i].IsVB())
                    for (auto& b : held)
                        if (win::NamesMatch(b.second, d.targets[i].name)) { busy = true; bus = b.first; break; }
                if (!busy) { i++; continue; }
                if (busyLogged_.insert(d.targets[i].id).second)
                    Log(d.targets[i].name + L" is used by Voicemeeter (A" + Num(bus + 1) +
                        L") - the plugin leaves its Windows format alone");
                d.targets.erase(d.targets.begin() + i);
            }
        }

        if (d.chain.empty()) {
            for (auto& e : d.targets) d.chain += (d.chain.empty() ? L"" : L" -> ") + e.name;
            if (d.chain.empty() && havePrimary) d.chain = primary.name;
        }
        if (d.vm) d.chain += std::wstring(L"  [") + win::Voicemeeter::TypeName(vm.Type()) + L"]";

        // The device that is heard: AIMP's own device, the ASIO driver, or the hardware on Voicemeeter's A1
        if (havePrimary) { d.aimpName = primary.name; d.aimpId = primary.id; }
        if (havePrimary && !primary.IsVB()) { d.endName = primary.name; d.endId = primary.id; }
        if (d.mode == OutputMode::Asio && !d.asio.name.empty() && !vbAsio) { d.endName = d.asio.name; d.endId.clear(); }
        if (d.vm) {
            d.endName.clear();
            d.endId.clear();
            auto buses = vm.BusDevices(false);
            if (!buses.empty()) {
                d.endName = buses.front().second;
                for (auto& e : d.targets)
                    if (!e.IsVB() && win::NamesMatch(d.endName, e.name)) { d.endId = e.id; break; }
            }
        }

        if (sharedLike && !havePrimary && d.targets.empty()) {
            d.info = L"No Windows output device found (check the 'Windows device' setting)";
            return d;  // rate = 0 -> counts as failure
        }
        if (d.targets.empty() && !d.vmEngine && !d.asioDirect) {
            // Nothing to switch: AIMP opens the device itself
            d.rate = t.rate;
            d.currentRate = t.rate;
            return d;
        }

        // Current format of every target, read once (each read opens the device list again)
        std::vector<WAVEFORMATEXTENSIBLE> formats(d.targets.size());
        std::vector<bool> known(d.targets.size());
        for (size_t i = 0; i < d.targets.size(); i++) known[i] = win::GetDeviceFormat(d.targets[i].id, formats[i]);

        // Choose a rate every device supports
        auto supports = [&](size_t i, uint32_t hz) {
            if (d.targets[i].IsVB()) return true;  // virtual drivers accept any rate
            if (!known[i]) return false;
            // -1 = unknown (e.g. held exclusively by Voicemeeter) -> try anyway
            return win::SupportsExclusive(d.targets[i].id, win::WithRate(formats[i], hz)) != 0;
        };
        for (uint32_t c : Candidates(t.rate)) {
            if (d.vmEngine && !win::Voicemeeter::EngineSupports(c)) continue;
            bool ok = true;
            for (size_t i = 0; i < d.targets.size(); i++) if (!supports(i, c)) { ok = false; break; }
            if (ok) { d.rate = c; break; }
        }
        if (!d.rate && !d.targets.empty()) {  // fallback: only the last device (AIMP's target) must support it
            for (uint32_t c : Candidates(t.rate))
                if (supports(d.targets.size() - 1, c) && (!d.vmEngine || win::Voicemeeter::EngineSupports(c))) { d.rate = c; break; }
        }
        if (!d.rate) return d;

        // Bit depth (optional, off by default): the default format of the devices AIMP plays into in shared
        // mode follows the track as well - 16 -> 16 bit, 24 -> 24 bit (packed or in 32 bits), 32 -> 32 bit,
        // if the driver supports it; otherwise the depth stays. Not for Voicemeeter: it mixes in float anyway.
        d.bits.assign(d.targets.size(), std::pair<WORD, WORD>(0, 0));
        if (cfg_.matchBitDepth && sharedLike && !d.vm && t.bits && !t.lossy) {
            std::vector<std::pair<WORD, WORD>> wanted;
            if (t.bits <= 16) wanted = {{16, 16}};
            else if (t.bits <= 24) wanted = {{24, 24}, {32, 24}, {32, 32}};  // 32 bit carries 24 bit unchanged
            else wanted = {{32, 32}};
            for (size_t i = 0; i < d.targets.size(); i++) {
                if (d.targets[i].IsVB() || !known[i]) continue;
                for (auto& w : wanted) {
                    WAVEFORMATEXTENSIBLE f = win::WithBits(win::WithRate(formats[i], d.rate), w.first, w.second);
                    if (win::SameFormat(f, win::WithRate(formats[i], d.rate)) ||
                        win::SupportsExclusive(d.targets[i].id, f) == 1) {
                        d.bits[i] = w;
                        break;
                    }
                }
            }
        }

        // Does anything have to change at all?
        for (size_t i = 0; i < d.targets.size(); i++) {
            if (!known[i]) continue;
            if (!d.currentRate) d.currentRate = formats[i].Format.nSamplesPerSec;
            if (formats[i].Format.nSamplesPerSec != d.rate) d.apply = true;
            if (d.bits[i].first && !win::SameFormat(formats[i], Target(formats[i], d, i))) d.apply = true;
        }
        if (d.vmEngine) {
            uint32_t known = vmSet_ ? vmSet_ : vm.PreferredRate();
            if (known) d.currentRate = known;
            if (known != d.rate) d.apply = true;
        }
        if (d.asioDirect && asioRateSet_ != d.rate) d.apply = true;
        if (d.asioDirect && !d.currentRate && asioRateSet_) d.currentRate = asioRateSet_;
        d.stopPlayback = d.apply;
        return d;
    }

    // 2.8 (ASIO / WASAPI exclusive / DirectSound, AIMP's rate changed on the fly): the rates AIMP itself should
    // play with, best first. Devices the plugin switches for this output (Voicemeeter, DirectSound's device,
    // extra devices) run at d.rate, and AIMP has to take exactly that rate - with Voicemeeter's ASIO driver a
    // different rate even plays at the wrong speed. Otherwise AIMP opens the device itself: the track's rate,
    // its multiples, then lower rates of the same family (192 -> 96 -> 48 kHz) for a device that cannot play
    // the track's rate. WASAPI exclusive: the device is asked which of them it supports (unknown = try).
    std::vector<uint32_t> LiveRates(const Decision& d, uint32_t hz) const {
        if (!d.rate) return {};
        if (d.vm && !d.vmEngine && d.mode == OutputMode::Asio) {  // Voicemeeter's ASIO driver runs at its engine rate
            uint32_t e = win::Voicemeeter::Get().EngineRate();
            if (e) return {e};
        }
        if (!d.targets.empty() || d.vm || d.asioDirect) return {d.rate};
        std::vector<uint32_t> c = Candidates(hz);
        uint32_t base = (hz % 44100 == 0) ? 44100 : (hz % 48000 == 0) ? 48000 : 0;
        if (base)
            for (uint32_t r = hz / 2; r >= base && r % base == 0; r /= 2) c.push_back(r);
        if (d.mode == OutputMode::Exclusive && !d.aimpId.empty()) {
            WAVEFORMATEXTENSIBLE f;
            if (win::GetDeviceFormat(d.aimpId, f)) {
                std::vector<uint32_t> ok;
                for (uint32_t r : c)
                    if (win::SupportsExclusive(d.aimpId, win::WithRate(f, r)) != 0) ok.push_back(r);
                if (!ok.empty()) c = ok;
            }
        }
        return c;
    }

    // New default format of target i: the chosen rate, and the bit depth if it follows the track
    static WAVEFORMATEXTENSIBLE Target(const WAVEFORMATEXTENSIBLE& cur, const Decision& d, size_t i) {
        WAVEFORMATEXTENSIBLE f = win::WithRate(cur, d.rate);
        if (i < d.bits.size() && d.bits[i].first) f = win::WithBits(f, d.bits[i].first, d.bits[i].second);
        return f;
    }

    // Returns the resulting output rate (0 = error)
    // settle = false: the caller waits anyway (AIMP's own rate is changed right afterwards), no extra pause
    uint32_t Apply(const Decision& d, bool settle = true) {
        last = Times();
        int64_t step = NowMs();
        bool ok = true, formatsChanged = false;
        // 2.8.1 (P8): Windows needs about a third of a second per device, so all devices that change are
        // switched at the same time (one thread each); the log lines keep the order of the devices
        struct Change {
            const win::Endpoint* e;
            WAVEFORMATEXTENSIBLE f, want;
            bool r = false;
            int64_t callMs = 0, ms = 0;  // Windows' call / until the new format is reported
        };
        std::vector<Change> changes;
        for (size_t i = 0; i < d.targets.size(); i++) {
            const win::Endpoint& e = d.targets[i];
            WAVEFORMATEXTENSIBLE f;
            if (!win::GetDeviceFormat(e.id, f)) continue;
            WAVEFORMATEXTENSIBLE want = Target(f, d, i);
            if (win::SameFormat(f, want)) continue;
            if (!original_.count(e.id)) original_[e.id] = f;
            Change c;
            c.e = &e;
            c.f = f;
            c.want = want;
            changes.push_back(c);
        }
        if (changes.size() == 1) {
            int64_t s = NowMs();
            changes[0].r = win::SetDeviceFormat(changes[0].e->id, changes[0].want, &changes[0].callMs);
            changes[0].ms = NowMs() - s;
        } else if (changes.size() > 1) {
            std::vector<std::thread> th;
            for (auto& c : changes)
                th.emplace_back([&c] {
                    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    int64_t s = NowMs();
                    c.r = win::SetDeviceFormat(c.e->id, c.want, &c.callMs);
                    c.ms = NowMs() - s;
                    if (SUCCEEDED(co)) CoUninitialize();
                });
            for (auto& t : th) t.join();
        }
        for (auto& c : changes) {
            ok &= c.r;
            formatsChanged |= c.r;
            bool bitsChange = win::ValidBits(c.f) != win::ValidBits(c.want) || win::IsFloat(c.f) != win::IsFloat(c.want);
            Log(c.e->name + L": " + Khz(c.f.Format.nSamplesPerSec) + (bitsChange ? L" / " + win::BitsText(c.f) : std::wstring()) +
                L" -> " + Khz(d.rate) + (bitsChange ? L" / " + win::BitsText(c.want) : std::wstring()) + (c.r ? L"  OK" : L"  FAILED") + L" (" + Num(c.ms) + L" ms, Windows call " + Num(c.callMs) + L" ms)");
        }
        uint32_t result = ok ? d.rate : 0;
        last.devices = NowMs() - step;
        step = NowMs();

        if (d.asioDirect) {
            win::AsioSession s(d.asio);
            if (!s.Ok()) {
                asioBusy_ = true;  // do not try again for every track
                result = 0;
            } else {
                uint32_t target = 0;
                for (uint32_t c : Candidates(d.rate)) if (s.Can(c)) { target = c; break; }
                uint32_t before = s.Rate();
                if (target && (before == target || s.SetRate(target))) {
                    asioRateSet_ = target;
                    result = target;
                    Log(L"ASIO " + d.asio.name + L": " + Khz(before) + L" -> " + Khz(target));
                } else {
                    Log(L"ASIO " + d.asio.name + L": rate " + Khz(d.rate) + L" not possible");
                    result = 0;
                }
            }
        }

        last.asio = NowMs() - step;
        step = NowMs();

        if (d.vm) {
            auto& vm = win::Voicemeeter::Get();
            // Remember the rate Voicemeeter ran with before the plugin touched it (restored when AIMP
            // closes). The engine's real rate is reliable; Option.sr can report stale values.
            if (!vmOriginalRate_ && !vmSet_) {
                vmOriginalRate_ = vm.EngineRate();
                if (!vmOriginalRate_) vmOriginalRate_ = vm.PreferredRate();
                if (vmOriginalRate_) Log(L"Voicemeeter: engine rate before this session " + Khz(vmOriginalRate_));
            }
            vmUsed_ = true;
            bool engineChange = d.vmEngine && vmSet_ != d.rate;
            if (engineChange) {
                if (vm.SetFloat("Option.sr", (float)d.rate)) vmSet_ = d.rate;
                else Log(L"Voicemeeter: could not set Option.sr");
                if (cfg_.vmAsioRate) vm.FollowEngineRateOnAsio();
            }
            // The engine has to restart to use a new rate or to reopen devices with a new format
            if (engineChange || formatsChanged) {
                int64_t t0 = NowMs();
                vm.Restart();
                if (d.vmEngine) {
                    bool ready = vm.WaitForEngine(d.rate, timing_.vmFirstPollMs, timing_.vmTimeoutMs);
                    uint32_t eng = vm.EngineRate();
                    Log(L"Voicemeeter: engine restarted" +
                        (ready ? L" with " + Khz(eng) + L" in " + Num(NowMs() - t0) + L" ms"
                               : L" (reports A1 at " + (eng ? Khz(eng) : std::wstring(L"?")) +
                                     L" - does the A1 device support the rate?)"));
                } else {
                    SleepMs(timing_.vmRestartMs);
                    Log(L"Voicemeeter: engine restarted");
                }
            }
        }
        last.vm = NowMs() - step;
        step = NowMs();
        if (settle) SleepMs(timing_.settleMs);
        last.settle = NowMs() - step;
        return result;
    }

    // Something was changed by the plugin that Shutdown would put back (idle restore)
    bool HasChanges() const {
        return !original_.empty() || (vmUsed_ && vmSet_ && vmOriginalRate_ && vmSet_ != vmOriginalRate_) ||
               win::Voicemeeter::Get().AsioRateOriginal() >= 0;
    }

    // When AIMP closes: restore the original formats (if enabled). force: always (idle restore, 2.8.1)
    void Shutdown(bool force = false) {
        if (cfg_.restoreOnExit || force) {
            auto& vm = win::Voicemeeter::Get();
            bool vmRunning = vmUsed_ && vm.Running();
            bool engine = vmRunning && vmOriginalRate_ && vmSet_ && vmSet_ != vmOriginalRate_;
            if (engine && !vm.SetFloat("Option.sr", (float)vmOriginalRate_)) {
                Log(L"Voicemeeter: could not set Option.sr back");
                engine = false;
            }
            // Option.ASIOsr as well (set by "ASIO device on A1 follows the engine rate"); the engine
            // restart below makes Voicemeeter use it
            bool asio = vm.AsioRateOriginal() >= 0 && vm.Running() && vm.RestoreAsioRate();
            bool restored = false;
            for (auto& o : original_) {
                bool r = win::SetDeviceFormat(o.first, o.second);
                restored |= r;
                Log(L"Restored: " + Khz(o.second.Format.nSamplesPerSec) + (r ? L"" : L" (failed)"));
            }
            // Voicemeeter has to reopen its devices (A1-A5) after their Windows format changed - otherwise
            // a WDM output stays red unless "Auto restart audio engine" is ticked in Voicemeeter.
            // Wait until the engine runs again: logging out right away can drop the pending command.
            if ((vmRunning || asio) && (engine || restored || asio)) {
                int64_t t0 = NowMs();
                vm.Restart();
                uint32_t target = engine ? vmOriginalRate_ : 0;
                bool ready = target ? vm.WaitForEngine(target, timing_.vmFirstPollMs, (std::max)(timing_.vmTimeoutMs, 3000))
                                    : (SleepMs((std::max)(timing_.vmRestartMs, 400)), true);
                if (engine)
                    Log(L"Voicemeeter: engine rate restored to " + Khz(vmOriginalRate_) +
                        (ready ? L" in " + Num(NowMs() - t0) + L" ms" : std::wstring(L" (engine did not confirm the rate)")));
                else
                    Log(restored ? L"Voicemeeter: engine restarted after restoring the device formats"
                                 : L"Voicemeeter: engine restarted after restoring Option.ASIOsr");
            }
        }
        win::Voicemeeter::Get().ForgetAsioRate();
        original_.clear();
        vmOriginalRate_ = 0;
        vmSet_ = 0;
        vmUsed_ = false;
    }

    // ---- Session across a restart by the plugin (ASIO / exclusive / DirectSound)
    // AIMP is closed only to apply the new rate: the devices must stay at that rate (no restore), and the
    // next AIMP process has to know the rates from before the session so that the real "AIMP closes"
    // still restores them. Stored in the settings file, section [Session].
    void SaveSession() {
        Ini& ini = Ini::Get();
        const tstring sec = AR_T("Session");
        ini.Clear(sec);
        int n = 0;
        for (auto& o : original_) {
            static const char* hex = "0123456789abcdef";
            std::wstring blob;
            const unsigned char* b = (const unsigned char*)&o.second;
            for (size_t i = 0; i < sizeof(o.second); i++) { blob += (wchar_t)hex[b[i] >> 4]; blob += (wchar_t)hex[b[i] & 15]; }
            ini.Set(sec, L"Device" + Num(n++), o.first + L"|" + blob);
        }
        ini.Set(sec, AR_T("VoicemeeterOriginal"), (long long)vmOriginalRate_);
        ini.Set(sec, AR_T("VoicemeeterSet"), (long long)vmSet_);
        ini.SetBool(sec, AR_T("VoicemeeterUsed"), vmUsed_);
        float asioSr = win::Voicemeeter::Get().AsioRateOriginal();
        if (asioSr >= 0) ini.Set(sec, AR_T("VoicemeeterAsioOriginal"), (long long)asioSr);
        ini.Set(sec, AR_T("Saved"), (long long)time(nullptr));
        ini.Save();
        Log(L"Restart: kept the current rates, remembered " + Num(n) + L" original device format(s)" +
            (vmOriginalRate_ ? L" and Voicemeeter " + Khz(vmOriginalRate_) : std::wstring()));
        original_.clear();
        vmOriginalRate_ = vmSet_ = 0;
        vmUsed_ = false;
        win::Voicemeeter::Get().ForgetAsioRate();
    }

    // Takes over the session of the AIMP process before a restart by the plugin (and removes it from the
    // file). fresh = the restart just happened, so Voicemeeter still runs at the rate the plugin set.
    void LoadSession(bool fresh) {
        Ini& ini = Ini::Get();
        const tstring sec = AR_T("Session");
        if (!ini.Int(sec, AR_T("Saved"), 0)) return;
        int n = 0;
        for (int i = 0; i < 64; i++) {
            std::wstring v = ini.Str(sec, L"Device" + Num(i));
            size_t bar = v.find(L'|');
            if (v.empty() || bar == std::wstring::npos || v.size() - bar - 1 != sizeof(WAVEFORMATEXTENSIBLE) * 2) continue;
            WAVEFORMATEXTENSIBLE f;
            unsigned char* b = (unsigned char*)&f;
            auto nib = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1; };
            bool valid = true;
            for (size_t k = 0; k < sizeof(f) && valid; k++) {
                int hi = nib(v[bar + 1 + 2 * k]), lo = nib(v[bar + 2 + 2 * k]);
                valid = hi >= 0 && lo >= 0;
                b[k] = (unsigned char)(hi << 4 | lo);
            }
            if (!valid || !win::PlausibleFormat(f)) continue;  // damaged entry: never hand it to Windows
            if (!original_.count(v.substr(0, bar))) original_[v.substr(0, bar)] = f;
            n++;
        }
        if (!vmOriginalRate_) vmOriginalRate_ = (uint32_t)ini.Int(sec, AR_T("VoicemeeterOriginal"), 0);
        long long asioSr = ini.Int(sec, AR_T("VoicemeeterAsioOriginal"), -1);
        if (asioSr >= 0 && asioSr <= 1000) win::Voicemeeter::Get().SetAsioRateOriginal((float)asioSr);
        if (fresh) {
            vmSet_ = (uint32_t)ini.Int(sec, AR_T("VoicemeeterSet"), 0);
            vmUsed_ = ini.Bool(sec, AR_T("VoicemeeterUsed"), false);
        }
        ini.Clear(sec);
        ini.Save();
        Log(L"Restart: took over " + Num(n) + L" original device format(s)" +
            (vmOriginalRate_ ? L" and Voicemeeter " + Khz(vmOriginalRate_) : std::wstring()) + L" from before the restart");
    }

    tstring VoicemeeterStatus() {
        auto& vm = win::Voicemeeter::Get();
        if (!vm.Installed()) return tstring();  // not mentioned at all where it is not installed
        long t = vm.Type();
        if (!t) return L"installed, not running";
        uint32_t eng = vm.EngineRate();
        tstring s = std::wstring(win::Voicemeeter::TypeName(t)) + L", engine " + (eng ? Khz(eng) : std::wstring(L"?"));
        for (auto& b : vm.BusDevices(true)) s += L", A" + Num(b.first + 1) + L": " + b.second;
        return s;
    }

#else
    void ResetAsio() {}
    void ResyncVoicemeeter() {}

    // ---------------------------------------------------------------- Linux
    Decision Prepare(const TrackInfo& t) {
        Decision d;
        d.mode = Resolve(t.aimpOutput);
        d.rate = t.rate;
        d.endName = t.aimpOutput;
        if (d.mode == OutputMode::PipeWire) {
            d.chain = "PipeWire (clock.force-rate)";
            // what PipeWire really runs at: it may have been changed or restarted meanwhile (e.g. while the plugin
            // was switched off); the rate set last if it cannot be read
            uint32_t now = 0;
            d.currentRate = lnx::PipeWire::ReadForced(now) ? now : pw_.Forced();
            d.apply = d.currentRate != t.rate;
            d.stopPlayback = false;  // PipeWire switches the graph while playing
        } else {
            d.currentRate = t.rate;
            d.info = "ALSA direct: AIMP opens the device itself";
        }
        return d;
    }

    uint32_t Apply(const Decision& d, bool settle = true) {
        (void)settle;
        if (d.mode != OutputMode::PipeWire) return d.rate;
        if (!lnx::PipeWire::Available()) {
            Log("pw-metadata not found (is PipeWire installed?)");
            return 0;
        }
        last = Times();
        int64_t step = NowMs();
        uint32_t before = pw_.Forced();
        bool ok = pw_.ForceRate(d.rate);
        Log("PipeWire: clock.force-rate " + (before ? Khz(before) : std::string("auto")) + " -> " + Khz(d.rate) +
            (ok ? "  OK" : "  FAILED"));
        last.devices = NowMs() - step;
        step = NowMs();
        SleepMs(timing_.settleMs);
        last.settle = NowMs() - step;
        return ok ? d.rate : 0;
    }

    bool HasChanges() const { return pw_.Forced() != 0; }

    void Shutdown(bool force = false) {
        (void)force;
        if (pw_.Forced()) {
            pw_.Release();
            Log("PipeWire: clock.force-rate released");
        }
    }

    tstring VoicemeeterStatus() { return tstring(); }
    void SaveSession() {}  // Linux: AIMP is never restarted by the plugin
    void LoadSession(bool) {}
#endif
};

}  // namespace ar
