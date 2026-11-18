// Connects AIMP with the RateController: collects events on the main thread, performs the
// switching on a worker thread and keeps the statistics.
#pragma once
#include <condition_variable>
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#include <functional>

#include "AimpOutput.h"
#include "BitPerfect.h"
#include "AimpUtil.h"
#include "Hardware.h"
#include "RateController.h"
#include "Rules.h"
#include "Updater.h"
#include "apiFileManager.h"
#include "apiMessages.h"
#include "apiPlayer.h"
#include "apiPlaylists.h"
#include "apiThreading.h"
#ifdef _WIN32
#include <map>
#include "win/LiveRate.h"
#include "win/Restart.h"
#endif

namespace ar {

class Task : public ComObject<IAIMPTask> {
    std::function<void()> fn_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPTask); }

public:
    explicit Task(std::function<void()> f) : fn_(std::move(f)) {}
    void WINAPI Execute(IAIMPTaskOwner*) override { fn_(); }
};

struct Job {
    TrackInfo track;
    Config cfg;
    tstring file;
    Ptr<IAIMPPlaylistItem> item;
    unsigned gen = 0;
    bool selfRestart = false;   // playback restarted by the plugin itself (not counted)
    bool outputChange = false;  // checked again because an output changed (B5)
};

class Engine {
public:
    Ptr<IAIMPServicePlayer> player;
    Ptr<IAIMPServiceThreads> threads;
    Config cfg;                // read/written on the main thread only
    Stats stats;
    Hardware hw;               // detected once at start-up
    Updater updater;           // About tab: update check (main thread)
    AimpOutputSync aimpSync;   // main thread only
    BitPerfect bitPerfect;     // main thread only
    std::function<void()> onStatsChanged;  // main thread; refreshes the settings page
    std::function<void()> onEnabledChanged;  // main thread; the plugin was switched on or off (menu, page)
    std::function<void()> beforeRestart;     // main thread, AIMP is about to restart: remember the open windows

private:
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> quit_{false};
    double pendingSeek_ = 0;  // AIMP 4: position to seek to once the track is open (main thread)
    std::atomic<bool> selfRestart_{false};  // AIMP is being closed by the plugin to apply a new rate
    std::thread watcher_;                   // update: waits for AIMP to install the new plugin files
    std::atomic<bool> watchStop_{false};
    bool sessionLoaded_ = false;            // worker only
    bool waitForVm_ = false;                // worker only: switch as soon as Voicemeeter runs
    bool has_ = false;
    std::atomic<bool> done_{false};
    Job job_;
    std::atomic<unsigned> gen_{0};
    // Protection against endless loops if a device ignores the switch
    tstring lastFile_;
    uint32_t lastRate_ = 0;
    int64_t lastTime_ = 0;
    tstring restartFile_;  // main thread only: file the plugin is restarting right now
    bool patchesHandedOver_ = false;  // main thread: a restart helper writes the bit-perfect values
    bool resumed_ = false;
    bool waitingForPlay_ = false;  // main thread only: a track was loaded while AIMP was not playing
    tstring handledFile_;          // main thread only: file of the last track the plugin looked at
    Config shutdownCfg_;

    // ---- B5: outputs that change while a track plays (main thread unless noted)
    tstring lastOutput_;           // AIMP's output when the current track was handled
    uint32_t lastAimpRate_ = 0;    // AIMP's own output rate (AIMPSoundOut\DeviceFreq) at that time
    int outputRechecks_ = 0;       // checks of the current track because of a change (at most 3 per track)
    bool pendingOutputChange_ = false;
    bool handledSkipped_ = false;  // the current track is a stream or an exception: nothing to check again
    std::atomic<bool> applying_{false};     // worker is switching right now
    std::atomic<int64_t> lastApplyEnd_{0};  // NowMs() when the worker last switched something
    std::atomic<int64_t> winChangedAt_{0};  // Windows: last change of a device (DeviceWatcher, another thread)
    std::atomic<int> winWhat_{0};
    std::atomic<int> vmChanged_{0};         // worker: Voicemeeter's A1-A5 devices or engine rate changed
    // worker only: the Voicemeeter state after the last switch, compared every 2 s while AIMP plays into it
    bool vmWatch_ = false;
    tstring vmBuses_;
    uint32_t vmRate_ = 0;
    // 2.8: AIMP's rate list per AIMP output, as read from its preferences (worker only)
    std::map<tstring, std::vector<uint32_t>> aimpLists_;
    std::atomic<bool> livePending_{false};  // AIMP's preferences were open: switch when they are closed (Tick)
    int liveFailures_ = 0;  // worker: changes in a row where AIMP's preferences could not be used (3 = given up)

    void OnMain(std::function<void()> f, bool wait) {
        if (quit_) return;  // Finalize waits on the main thread -> do not block
        Ptr<IAIMPTask> t;
        Task* raw = new Task(std::move(f));
        raw->AddRef();
        *t.Out() = raw;
        threads->ExecuteInMainThread(t.Get(), wait ? AIMP_SERVICE_THREADS_FLAGS_WAITFOR : 0);
    }

public:
    // AIMP's current output (AIMP 4: from its settings); main thread
    tstring AimpOutput() {
        Ptr<IAIMPPropertyList> pl;
        if (Failed(player->QueryInterface(IID_IAIMPPropertyList, pl.OutV())) || !pl) return tstring();
        Ptr<IAIMPString> s;
        if (Failed(pl->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPString, s.OutV())) || !s)
            return ConfiguredAimpOutput();  // AIMP 4
        return FromString(s.Get());
    }

private:
    // Prepares the restart with a new output rate (main thread): resume file, helper process and,
    // in seamless mode, waits until the still image is on screen. AIMP itself is closed
    // asynchronously afterwards (see Handle). false = not possible.
    bool RestartAimpForRate(const tstring& file, double pos, uint32_t hz, bool seamless) {
#ifdef _WIN32
        if ((hz && aimpSync.rateKey.empty()) || aimpSync.profileDir.empty()) return false;
        ResumeInfo r;
        r.file = file;
        r.position = pos;
        r.rate = hz;
        r.time = (long long)time(nullptr);
        r.windows = win::SaveWindowsLine();
        aimpSync.SaveResume(r);
        bool background = !r.windows.empty() && r.windows[0] == L'0';
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, win::ReadyEventName(GetCurrentProcessId()).c_str());
        tstring patches = BpPatchText(bitPerfect.Pending(cfg));
        if (!win::LaunchRestartHelper(hz, aimpSync.profileDir + AR_T("AIMP.ini"), aimpSync.rateKey, seamless, background, patches)) {
            if (ready) CloseHandle(ready);
            return false;
        }
        if (ready) {
            // The helper signals when the still image is on screen. A slow start (e.g. a virus scanner
            // checking rundll32) must not close AIMP before that, or the restart is visible.
            if (WaitForSingleObject(ready, seamless ? 6000 : 1000) != WAIT_OBJECT_0 && seamless)
                Log(AR_T("Restart helper did not answer in time - AIMP closes without still image"));
            CloseHandle(ready);
        }
        patchesHandedOver_ = true;
        if (beforeRestart) beforeRestart();
        Log(hz ? AR_T("Restarting AIMP with ") + Khz(hz) + AR_T(" (") + aimpSync.rateKey + AR_T(")")
               : tstring(AR_T("Restarting AIMP to apply the bit-perfect mode")));
        return true;
#else
        (void)file; (void)pos; (void)hz; (void)seamless;
        return false;
#endif
    }

    void Handle(Job& j, RateController& rc) {
        waitForVm_ = false;
        vmWatch_ = false;
        if (j.outputChange) {
            lastFile_.clear();  // the situation changed: switching again is not "without effect"
            rc.ResyncVoicemeeter();
        }
        if (!sessionLoaded_) {
            // After a restart by the plugin: take over the original rates of the previous AIMP process
            sessionLoaded_ = true;
            OnMain([this, &rc] { rc.LoadSession(aimpSync.resume.time != 0); }, true);
        }
        Timing tm = Timing::For(hw, j.cfg.autoTiming, j.cfg.settleMs);
        rc.SetConfig(j.cfg, tm);
        OutputMode mode = rc.Resolve(j.track.aimpOutput);

        // Which rate does AIMP itself play with? (AIMPSoundOut\DeviceFreq)
        uint32_t aimpRate = 0;
        OnMain([this, &aimpRate] { lastAimpRate_ = aimpRate = aimpSync.AimpRate(); }, true);

        // ASIO / WASAPI exclusive / DirectSound: AIMP plays with its own fixed rate there. Since 2.8 the
        // plugin changes that rate on the fly (win/LiveRate.h): devices first (Voicemeeter, DirectSound's
        // device), then AIMP, then AIMP's setting is checked. The legacy option restarts AIMP instead.
        // Where AIMP's rate cannot change, the devices stay at AIMP's rate so the pitch is always right.
        bool restartOutput = NeedsAimpRestart(mode);
#ifdef _WIN32
        bool canRestart = restartOutput && j.cfg.legacyRestart && aimpRate;
        bool live = restartOutput && !canRestart && aimpRate;
#else
        bool canRestart = false, live = false;
#endif
        bool passive = restartOutput && !canRestart && !live;
        TrackInfo t = j.track;
        tstring note;
#ifdef _WIN32
        if (live) {
            bool open = false;
            OnMain([&open] { open = win::LiveRate::PreferencesOpen(); }, true);
            if (open) {  // never touch the window the user works in
                live = false;
                passive = true;
                livePending_ = true;
                note = tstring(AR_T("AIMP's preferences are open - AIMP's rate follows when they are closed; devices stay at AIMP's rate (")) +
                       Khz(aimpRate) + AR_T(")");
            } else if (liveFailures_ >= 3) {  // this AIMP's preferences cannot be used (see the log)
                live = false;
                passive = true;
                note = tstring(AR_T("AIMP's rate cannot be changed in this AIMP - devices stay at AIMP's rate (")) + Khz(aimpRate) + AR_T(")");
            }
        }
#endif
        if (passive) {
            if (!aimpRate) {
                stats.Update([&](Stats& s) {
                    s.modeText = ModeName(mode);
                    s.note = AR_T("AIMP's output rate unknown - nothing changed");
                    if (!j.selfRestart) s.skipped++;
                });
                if (!j.selfRestart)
                    Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | ") + ModeName(mode) +
                        AR_T(" | AIMP's output rate unknown - nothing changed"));
                return;
            }
            t.rate = aimpRate;
        }
#ifndef _WIN32
        // AIMP for Linux also resamples every track to its own fixed rate (AIMPSoundOut\DeviceFreq),
        // whatever the device does. Forcing PipeWire to the track's rate would resample a second time
        // (e.g. 96 -> 44.1 in AIMP, 44.1 -> 96 in PipeWire), so PipeWire follows AIMP's rate instead.
        if (!aimpRate) {
            stats.Update([&](Stats& s) {
                s.modeText = ModeName(mode);
                s.note = AR_T("AIMP's output rate unknown - nothing changed");
                if (!j.selfRestart) s.skipped++;
            });
            if (!j.selfRestart)
                Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | ") + ModeName(mode) +
                    AR_T(" | AIMP's output rate unknown - nothing changed"));
            return;
        }
        if (t.rate != aimpRate)
            note = tstring(AR_T("AIMP resamples to its own rate (")) + Khz(aimpRate) + AR_T(")") +
                   (mode == OutputMode::PipeWire ? AR_T(" - PipeWire follows AIMP") : AR_T(""));
        t.rate = aimpRate;
#endif

        // Highest rate (General tab): 192 -> 96, 176.4 -> 88.2 kHz etc.; AIMP resamples the track then.
        // Only where the plugin follows the track: AIMP's own fixed rate where it cannot change (preferences
        // open, Linux) is AIMP's setting, and the devices behind AIMP must keep running at it.
#ifdef _WIN32
        bool followsTrack = !passive;
#else
        bool followsTrack = false;
#endif
        uint32_t unlimited = t.rate, limited = followsTrack ? LimitRate(t.rate, j.cfg.maxRate) : t.rate;
        if (limited != t.rate) {
            note += tstring(note.empty() ? AR_T("") : AR_T("; ")) + AR_T("limited to ") + Khz(limited) + AR_T(" (highest rate setting)");
            t.rate = limited;
        }

        Decision d = rc.Prepare(t);
        std::vector<uint32_t> liveWanted;
        uint32_t liveTarget = 0;
#ifdef _WIN32
        // The rates AIMP should take; if AIMP's list for this output is known and offers none of them, AIMP
        // keeps its rate and the devices follow AIMP instead
        if (live && d.rate) {
            liveWanted = rc.LiveRates(d, t.rate);
            auto known = aimpLists_.find(j.track.aimpOutput);
            if (known != aimpLists_.end()) {
                for (uint32_t w : liveWanted)
                    if (std::find(known->second.begin(), known->second.end(), w) != known->second.end()) { liveTarget = w; break; }
                if (!liveTarget) {
                    live = false;
                    passive = true;
                    note = tstring(AR_T("AIMP does not offer ")) + Khz(liveWanted.empty() ? t.rate : liveWanted[0]) +
                           AR_T(" for this output - devices stay at AIMP's rate (") + Khz(aimpRate) + AR_T(")");
                    t.rate = aimpRate;
                    d = rc.Prepare(t);
                }
            } else if (!liveWanted.empty()) {
                liveTarget = liveWanted[0];
            }
        }
        if (passive && !d.vm) d.apply = d.stopPlayback = false;  // nothing to keep in sync
        if (d.vmNotRunning) {
            waitForVm_ = true;
            note += tstring(note.empty() ? AR_T("") : AR_T("; ")) + AR_T("Voicemeeter is not running - switching as soon as it starts");
        }
#endif
        tstring vmText = rc.VoicemeeterStatus();
        bool vmChain = false;
#ifdef _WIN32
        vmChain = d.vm;
#endif
        stats.Update([&](Stats& s) {
            s.modeText = ModeName(d.mode);
            if (j.cfg.mode == OutputMode::Auto) s.modeText = tstring(AR_T("Automatic -> ")) + s.modeText;
            s.chainText = d.chain;
            s.vmText = vmText;
            s.deviceRate = d.currentRate;
            s.aimpRate = aimpRate;
            s.note = note;
            s.mode = (int)d.mode;
            s.trackFile = j.file;
            s.endDevice = d.endName;
            s.endDeviceId = d.endId;
            s.aimpDevice = d.aimpName;
            s.aimpDeviceId = d.aimpId;
            s.limitedTo = limited != unlimited ? limited : 0;
            s.exception.clear();
            s.vmChain = vmChain;
            s.vmEngineRate = 0;
            s.hardwareText = hw.Describe() + AR_T(" -> ") + hw.SpeedName() + AR_T(" profile, ") + Num(tm.settleMs) +
                             AR_T(" ms settle time");
        });
        if (!j.selfRestart)
            Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | AIMP output: ") +
                (j.track.aimpOutput.empty() ? tstring(AR_T("?")) : j.track.aimpOutput) + AR_T(" | AIMP rate: ") +
                (aimpRate ? Khz(aimpRate) : tstring(AR_T("?"))) + AR_T(" | ") + ModeName(d.mode) +
                (d.chain.empty() ? tstring() : AR_T(" | ") + d.chain) + (note.empty() ? tstring() : AR_T(" | ") + note));
        if (!d.info.empty()) Log(d.info);
        if (!d.rate) {
            if (d.info.empty()) Log(AR_T("No suitable rate found for ") + Khz(t.rate));
            stats.Update([](Stats& s) { s.failures++; });
            return;
        }

        // AIMP itself has to play with the target rate -> otherwise restart it with the new rate
        bool aimpNeeds = canRestart && aimpRate != d.rate;
        if (aimpNeeds && aimpSync.resume.rate == d.rate && (long long)time(nullptr) - aimpSync.resume.time < 90) {
            Log(AR_T("  AIMP did not take ") + Khz(d.rate) + AR_T(" after the restart - no further restart"));
            aimpNeeds = false;
        }
        bool devApply = d.apply;
        if (aimpNeeds) d.apply = d.stopPlayback = true;
#ifdef _WIN32
        // Without restart: AIMP changes its output while the track keeps playing; playback only stops if
        // devices change as well (as with every other output), and AIMP takes the new rate meanwhile
        bool liveNeeds = live && liveTarget && liveTarget != aimpRate;
        if (liveNeeds) d.apply = true;
#else
        bool liveNeeds = false;
#endif
        if (!d.apply) {
            stats.Update([&](Stats& s) {
                if (!j.selfRestart) s.skipped++;
                if (!s.deviceRate) s.deviceRate = d.rate;
            });
            WatchVoicemeeter(d);
            return;
        }
        if (!aimpNeeds && j.file == lastFile_ && d.rate == lastRate_ && NowMs() - lastTime_ < 15000) {
            Log(AR_T("Switching to ") + Khz(d.rate) + AR_T(" has no effect - skipped for this track"));
            stats.Update([](Stats& s) { s.failures++; });
            WatchVoicemeeter(d);  // e.g. a device on A1 that keeps its format: Voicemeeter itself is still watched
            return;
        }
        if (gen_ != j.gen) return;  // another track is playing meanwhile

        Log(AR_T("  -> switching output to ") + Khz(liveNeeds && !devApply ? liveTarget : d.rate) +
            (aimpNeeds ? tstring(AR_T(" (AIMP: ")) + Khz(aimpRate) + AR_T(" -> ") + Khz(d.rate) + AR_T(", restart)")
             : liveNeeds ? tstring(AR_T(" (AIMP: ")) + Khz(aimpRate) + AR_T(" -> ") + Khz(liveTarget) + AR_T(", without restart)")
                         : tstring()));
        int64_t t0 = NowMs();
        double pos = 0;
        if (d.stopPlayback) {
            bool playing = true;
            OnMain([this, &pos, &playing] {
                playing = player->GetState() == AIMP_PLAYER_STATE_PLAYING;
                if (!playing) { waitingForPlay_ = true; return; }  // paused meanwhile: redo on play
                player->GetPosition(&pos);
                player->Stop();
            }, true);
            if (!playing) {
                Log(AR_T("  playback was paused meanwhile - switching when it continues"));
                return;
            }
        }
        applying_ = true;
        uint32_t result = devApply ? rc.Apply(d) : d.rate;
#ifdef _WIN32
        bool liveOk = true;
        uint32_t liveGot = 0;
        if (liveNeeds && !quit_ && gen_ == j.gen) {
            std::vector<uint32_t> want = liveWanted;
            if (devApply && result && result != d.rate) want = {result};  // e.g. an ASIO driver set by the plugin
            win::LiveRate::Result lr = SwitchAimpLive(want);
            if (!lr.list.empty()) aimpLists_[j.track.aimpOutput] = lr.list;
            liveOk = lr.ok;
            liveGot = lr.got ? lr.got : aimpRate;
            // A window or list that is not found (e.g. another AIMP version): after three times in a row the
            // plugin stops trying, instead of opening AIMP's preferences in vain for every track
            if (lr.ok || lr.open || lr.busy || !lr.list.empty()) liveFailures_ = 0;
            else if (!quit_ && ++liveFailures_ == 3)
                Log(AR_T("AIMP's rate could not be changed three times in a row - not tried again until AIMP restarts. "
                         "The legacy method (ASIO / Exclusive tab) restarts AIMP instead."));
            if (lr.ok) {
                Log(AR_T("  AIMP: ") + Khz(aimpRate) + AR_T(" -> ") + Khz(liveGot) + AR_T(" without restart (") + Num(lr.ms) +
                    AR_T(" ms)"));
                if (!devApply) result = liveGot;
                // Voicemeeter's ASIO driver runs at the engine rate, whatever AIMP asks for: both must match
                uint32_t eng = d.vm && d.mode == OutputMode::Asio ? win::Voicemeeter::Get().EngineRate() : 0;
                if (eng && eng != liveGot) {
                    Log(AR_T("  Voicemeeter's engine runs at ") + Khz(eng) + AR_T(", not ") + Khz(liveGot) + AR_T(" - AIMP follows Voicemeeter"));
                    win::LiveRate::Result again = SwitchAimpLive({eng});
                    if (again.got) liveGot = again.got;
                    liveOk = false;
                    result = 0;
                } else if (eng) {
                    Log(AR_T("  Voicemeeter's engine and AIMP: ") + Khz(eng));
                }
            } else {
                if (lr.open) livePending_ = true;
                Log(AR_T("  AIMP's rate stays at ") + Khz(liveGot) + AR_T(": ") + lr.why +
                    (lr.list.empty() ? tstring() : AR_T(" (AIMP offers ") + RateList(lr.list) + AR_T(")")));
                // The devices behind AIMP have to run at AIMP's rate (Voicemeeter's ASIO driver would play at
                // the wrong speed otherwise): back to it
                if (devApply && liveGot != d.rate) {
                    TrackInfo back = t;
                    back.rate = liveGot;
                    Decision b = rc.Prepare(back);
                    if (b.rate && b.apply) {
                        Log(AR_T("  devices back to AIMP's rate ") + Khz(liveGot));
                        rc.Apply(b);
                    }
                }
                result = 0;
            }
        }
#endif
        lastApplyEnd_ = NowMs();
        applying_ = false;
        int64_t dt = NowMs() - t0;
        lastFile_ = j.file;
        lastRate_ = d.rate;
        lastTime_ = NowMs();
        stats.Update([&](Stats& s) {
            if (result) {
                s.switches++;
                s.lastSwitchMs = dt;
                s.switchMsTotal += dt;
                s.deviceRate = result;
            } else {
                s.failures++;
            }
#ifdef _WIN32
            if (d.vm && devApply && d.vmEngine) s.vmRestarts++;
            if (liveNeeds) {
                if (liveGot) s.aimpRate = liveGot;
                if (liveOk) s.aimpLive++;
                else s.note = tstring(AR_T("AIMP's rate could not be changed - it stays at ")) + Khz(liveGot);
                if (!liveOk && devApply) s.deviceRate = liveGot;
            }
#endif
        });
        WatchVoicemeeter(d);
        if (quit_ || gen_ != j.gen) return;

        if (aimpNeeds) {
            tstring file = j.file;
            uint32_t hz = d.rate;
            bool seamless = j.cfg.seamlessRestart;
            bool restarting = false;
            OnMain([this, file, pos, hz, seamless, &restarting] { restarting = RestartAimpForRate(file, pos, hz, seamless); }, true);
            if (restarting) {
                selfRestart_ = true;  // keep the new rates when this AIMP closes (see Run)
                stats.Update([](Stats& s) { s.aimpRestarts++; });
                // Do not wait: while closing, AIMP calls Finalize, which waits for this worker
                OnMain([] {
                    auto sd = Service<IAIMPServiceShutdown>(IID_IAIMPServiceShutdown);
                    if (!sd || Failed(sd->Shutdown(AIMP_SERVICE_SHUTDOWN_FLAGS_CLOSE_APP | AIMP_SERVICE_SHUTDOWN_FLAGS_NO_CONFIRM)))
                        Log(AR_T("AIMP could not be closed"));
                }, false);
                return;  // continues after the start (ResumeAfterRestart)
            }
        }
        if (d.stopPlayback) {
            IAIMPPlaylistItem* item = j.item.Get();
            tstring file = j.file;
            float offset = pos > 1.0 ? (float)pos : 0.0f;  // continue at the same position
            OnMain([this, item, file, offset] {
                restartFile_ = file;
                PlayAt(item, file, offset, AIMP_SERVICE_PLAYER_FLAGS_PLAY_WITHOUT_ADDING_TO_PLAYLIST);
            }, true);
        }
    }

    // Worker: remembers Voicemeeter's devices and engine rate after a switch, to notice when they change (B5)
    void WatchVoicemeeter(const Decision& d) {
#ifdef _WIN32
        vmWatch_ = d.vm;
        if (!d.vm) return;
        auto& vm = win::Voicemeeter::Get();
        vmBuses_.clear();
        for (auto& b : vm.BusDevices(true)) vmBuses_ += Num(b.first) + AR_T("=") + b.second + AR_T(";");
        vmRate_ = d.vmEngine ? d.rate : 0;  // what the plugin set (Voicemeeter may report the old value for a moment)
        uint32_t eng = vm.EngineRate();
        stats.Update([&](Stats& s) { s.vmEngineRate = eng; });
#else
        (void)d;
#endif
    }

#ifdef _WIN32
    static tstring RateList(const std::vector<uint32_t>& l) {
        tstring r;
        for (uint32_t v : l) r += (r.empty() ? AR_T("") : AR_T(", ")) + Num(v);
        return r;
    }

    // Worker: AIMP's own rate without restart (win/LiveRate.h runs on the main thread, the worker waits)
    win::LiveRate::Result SwitchAimpLive(const std::vector<uint32_t>& wanted) {
        struct Wait {
            std::mutex m;
            std::condition_variable cv;
            bool done = false;
            win::LiveRate::Result r;
        };
        auto w = std::make_shared<Wait>();
        OnMain([this, w, wanted] {
            win::LiveRate::Get().Start(wanted, [this] { return aimpSync.LiveAimpRate(); },
                                       [this, w](const win::LiveRate::Result& r) {
                                           if (r.got) lastAimpRate_ = r.got;  // the plugin's own change (see Tick)
                                           std::lock_guard<std::mutex> l(w->m);
                                           w->r = r;
                                           w->done = true;
                                           w->cv.notify_all();
                                       });
        }, true);
        std::unique_lock<std::mutex> l(w->m);
        for (int64_t until = NowMs() + 15000; !w->done && !quit_ && NowMs() < until;)
            w->cv.wait_for(l, std::chrono::milliseconds(100));
        if (!w->done) {
            win::LiveRate::Result r;
            r.why = quit_ ? L"AIMP is closing" : L"no answer";
            return r;
        }
        return w->r;
    }
#endif

    // Worker, every 2 s while AIMP plays into Voicemeeter: did someone change A1-A5 or the engine rate?
    void CheckVoicemeeterChanged() {
#ifdef _WIN32
        auto& vm = win::Voicemeeter::Get();
        if (!vm.Running()) return;  // closed: handled when it comes back (waitForVm_) or with the next track
        tstring buses;
        for (auto& b : vm.BusDevices(true)) buses += Num(b.first) + AR_T("=") + b.second + AR_T(";");
        uint32_t sr = vmRate_ ? vm.PreferredRate() : 0;
        if (buses == vmBuses_ && sr == vmRate_) return;
        tstring what = buses != vmBuses_ ? tstring(AR_T("Voicemeeter output device")) : AR_T("Voicemeeter engine rate ") + Khz(sr);
        vmBuses_ = buses;
        vmRate_ = sr;
        vmWatch_ = false;  // until the check is done
        OnMain([this, what] { OutputChanged(what); }, false);
#endif
    }

    // Worker: Voicemeeter was not running for the current track - has it been started meanwhile?
    void CheckVoicemeeterStarted() {
#ifdef _WIN32
        if (!win::Voicemeeter::Get().Running()) return;
        waitForVm_ = false;
        lastFile_.clear();  // the same track again is not a switch "without effect" now
        Log(AR_T("Voicemeeter is running now - switching for the current track"));
        OnMain([this] { RecheckSamePlayback(); }, false);
#endif
    }

    void Run() {
#ifdef _WIN32
        // STA, because ASIO drivers are apartment-threaded COM objects without marshalling
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        RateController rc;
#ifdef _WIN32
        win::DeviceWatcher watcher(&winChangedAt_, &winWhat_);
        if (!watcher.Start()) Log(AR_T("Windows does not report device changes - only AIMP's own output is watched"));
#endif
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                auto ready = [&] { return quit_ || has_; };
                if (!waitForVm_ && !vmWatch_) cv_.wait(l, ready);
                else if (!cv_.wait_for(l, std::chrono::seconds(waitForVm_ ? 3 : 2), ready)) {
                    l.unlock();
                    if (waitForVm_) CheckVoicemeeterStarted();
                    else CheckVoicemeeterChanged();
                    continue;
                }
                if (quit_) break;
                j = std::move(job_);
                job_ = Job();
                has_ = false;
            }
            Handle(j, rc);
            if (!quit_) OnMain([this] { if (onStatsChanged) onStatsChanged(); }, false);
        }
        rc.SetConfig(shutdownCfg_, Timing::For(hw, shutdownCfg_.autoTiming, shutdownCfg_.settleMs));
        // The main thread waits in Stop() meanwhile, so the settings file can be written from here.
        // No track was handled since a restart by the plugin (e.g. the plugin was disabled meanwhile, or the
        // bit-perfect restart came from disabling it): the original rates of the previous AIMP are still in
        // the settings file - take them over now, so they are restored (or handed on to the next restart).
        if (!sessionLoaded_) {
            sessionLoaded_ = true;
            rc.LoadSession(aimpSync.resume.time != 0);
        }
        // Closed by the plugin for a new rate: keep the devices at that rate, remember the originals.
        if (selfRestart_) rc.SaveSession();
        else rc.Shutdown();
#ifdef _WIN32
        watcher.Stop();
        win::Voicemeeter::Get().Shutdown();
        if (SUCCEEDED(co)) CoUninitialize();
#endif
        done_ = true;
    }

public:
    void Start() {
        hw = Hardware::Detect();
        Timing tm = Timing::For(hw, cfg.autoTiming, cfg.settleMs);
        Log(AR_T("Hardware: ") + (hw.cpu.empty() ? tstring() : hw.cpu + AR_T(", ")) + hw.Describe() + AR_T(" -> ") +
            hw.SpeedName() + AR_T(" timing profile (settle ") + Num(tm.settleMs) + AR_T(" ms)"));
        stats.Update([&](Stats& s) {
            s.hardwareText = hw.Describe() + AR_T(" -> ") + hw.SpeedName() + AR_T(" profile, ") + Num(tm.settleMs) +
                             AR_T(" ms settle time");
        });
        worker_ = std::thread([this] { Run(); });
    }

    // ---- Update: AIMP installs the downloaded .aimppack after the user confirmed it. The running plugin
    // stays loaded, so it notices the new files on disk itself and restarts AIMP to load them.
    struct FileStamp {
        long long size = -1, time = 0, id = 0;
        bool operator!=(const FileStamp& o) const { return size != o.size || time != o.time || id != o.id; }
    };
    static FileStamp Stamp(const tstring& path) {
        FileStamp f;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
            f.size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
            f.time = ((long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        }
#else
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            f.size = (long long)st.st_size;
            f.time = (long long)st.st_mtime;
            f.id = (long long)st.st_ino;  // the new .so usually replaces the file (new inode)
        }
#endif
        return f;
    }
    static tstring PluginFile() {
#ifdef _WIN32
        return win::ModulePath(win::ThisModule());
#else
        Dl_info info;
        if (dladdr((void*)&Engine::Stamp, &info) && info.dli_fname) return info.dli_fname;
        return tstring();
#endif
    }

public:
    void WatchForUpdate() {
        if (watcher_.joinable()) return;  // already watching
        tstring path = PluginFile();
        if (path.empty()) return;
        FileStamp before = Stamp(path);
        Log(AR_T("Update: waiting for AIMP to install the new version (") + path + AR_T(")"));
        watchStop_ = false;
        watcher_ = std::thread([this, path, before] {
            int64_t end = NowMs() + 30 * 60 * 1000;  // give up after 30 minutes (installation cancelled)
            while (!watchStop_ && NowMs() < end) {
                for (int i = 0; i < 10 && !watchStop_; i++) SleepMs(100);
                if (watchStop_) return;
                if (Stamp(path) != before) {
                    SleepMs(1500);  // let AIMP finish writing all files of the package
                    if (!watchStop_) OnMain([this] { RestartForUpdate(); }, false);
                    return;
                }
            }
        });
    }

    // Main thread: continue the current track after the restart, keep the current rates, restart AIMP
    void RestartForUpdate() {
        tstring file;
        double pos = 0;
        if (player && player->GetState() == AIMP_PLAYER_STATE_PLAYING) {
            file = CurrentFile();
            player->GetPosition(&pos);
        }
        if (!file.empty() && !aimpSync.profileDir.empty()) {
            ResumeInfo r;
            r.file = file;
            r.position = pos;
            r.rate = 0;
            r.time = (long long)time(nullptr);
            aimpSync.SaveResume(r);
        }
        Log(AR_T("Update installed - restarting AIMP to load the new version"));
        updater.Installed();
        selfRestart_ = true;  // keep the rates; the new version takes the session over
        auto sd = Service<IAIMPServiceShutdown>(IID_IAIMPServiceShutdown);
        auto none = MakeString(tstring());
        if (!sd || Failed(sd->Restart(none.Get()))) {
            selfRestart_ = false;
            Log(AR_T("AIMP could not restart itself - please restart AIMP to finish the update"));
        }
    }

    // Main thread: restarts AIMP so the bit-perfect settings it only reads at start-up take effect
    // (Output tab -> "Restart AIMP now"). The track continues at the same position.
    void RestartForSettings() {
#ifdef _WIN32
        tstring file;
        double pos = 0;
        if (player && player->GetState() == AIMP_PLAYER_STATE_PLAYING) {
            file = CurrentFile();
            player->GetPosition(&pos);
        }
        if (!RestartAimpForRate(file, pos, 0, cfg.seamlessRestart)) {
            Log(AR_T("AIMP could not be restarted - the bit-perfect settings follow with the next start"));
            return;
        }
        selfRestart_ = true;  // keep the current rates; the new AIMP takes the session over
        auto sd = Service<IAIMPServiceShutdown>(IID_IAIMPServiceShutdown);
        if (!sd || Failed(sd->Shutdown(AIMP_SERVICE_SHUTDOWN_FLAGS_CLOSE_APP | AIMP_SERVICE_SHUTDOWN_FLAGS_NO_CONFIRM)))
            Log(AR_T("AIMP could not be closed"));
#endif
    }

    // Main thread, while AIMP closes: settings AIMP only reads at start-up are written into AIMP.ini
    // once it has closed (AIMP writes its own values first), unless a restart helper does it already
    void PatchAimpIniAtExit() {
#ifdef _WIN32
        if (patchesHandedOver_ || aimpSync.profileDir.empty()) return;
        auto p = bitPerfect.Pending(cfg);
        if (p.empty()) return;
        if (win::LaunchRestartHelper(0, aimpSync.profileDir + AR_T("AIMP.ini"), tstring(), false, true, BpPatchText(p), AR_T("-")))
            Log(AR_T("Bit-perfect mode: AIMP.ini is changed after AIMP has closed"));
#endif
    }

private:
    void StopWatcher() {
        watchStop_ = true;
        if (watcher_.joinable()) watcher_.join();
    }

public:
    // true = worker finished cleanly; false = still hanging (then deliberately not freed)
    bool Stop() {
        StopWatcher();
        {
            std::lock_guard<std::mutex> l(m_);
            shutdownCfg_ = cfg;
            quit_ = true;
            cv_.notify_all();
        }
        for (int i = 0; i < 160 && !done_; i++) SleepMs(50);  // max. 8 s (restoring incl. Voicemeeter restart)
        if (done_) worker_.join();
        else worker_.detach();
        return done_;
    }

    // Runs f on the main thread later (e.g. a dialog that must not block the current event)
    void Later(std::function<void()> f) { OnMain(std::move(f), false); }

    // Main thread, after AIMP_MSG_EVENT_LOADED: continue the track after a restart by the plugin
    void ResumeAfterRestart() {
        const ResumeInfo& r = aimpSync.resume;
        if (r.file.empty() || resumed_) return;
        resumed_ = true;
        Log(AR_T("Continuing after restart: ") + r.file + AR_T(" at ") + Num((long long)r.position) + AR_T(" s"));
        restartFile_ = r.file;
        DWORD flags = AIMP_SERVICE_PLAYER_FLAGS_PLAY_FROM_PLAYLIST | AIMP_SERVICE_PLAYER_FLAGS_PLAY_FROM_PLAYLIST_CAN_ADD;
        if (Failed(PlayAt(nullptr, r.file, (float)r.position, flags))) Log(AR_T("Continuing failed"));
    }

    // Main thread: plays a playlist item (or else a file) from the given position. AIMP 5.40+ starts
    // there directly (IAIMPServicePlayer2); AIMP 4 starts at the beginning and then seeks.
    HRESULT PlayAt(IAIMPPlaylistItem* item, const tstring& file, float offset, DWORD flags) {
        Ptr<IAIMPServicePlayer2> p2;
        if (offset > 0) player->QueryInterface(IID_IAIMPServicePlayer2, p2.OutV());
        auto s = MakeString(file);
        HRESULT hr = E_FAIL;
        if (item) hr = p2 ? p2->Play2(item, offset, 0) : player->Play2(item);
        else if (s) hr = p2 ? p2->Play4(s.Get(), offset, flags) : player->Play4(s.Get(), flags);
        // AIMP 4 opens the track asynchronously: seek now if possible, otherwise at the stream start
        if (Succeeded(hr) && !p2 && offset > 0) pendingSeek_ = Failed(player->SetPosition(offset)) ? offset : 0;
        return hr;
    }

    // Main thread: seek that PlayAt could not do yet (AIMP 4). finalTry = give up if it fails again.
    void ApplyPendingSeek(bool finalTry) {
        if (pendingSeek_ <= 0) return;
        double pos = pendingSeek_;
        if (Succeeded(player->SetPosition(pos))) pendingSeek_ = 0;
        else if (finalTry) {
            pendingSeek_ = 0;
            Log(AR_T("Could not continue at ") + Num((long long)pos) + AR_T(" s"));
        }
    }

    // Main thread: playback state changed. A track that AIMP loaded without playing it (e.g. the last
    // track restored paused at start-up) is handled as soon as playback really starts.
    // Also covers the track AIMP restores at start-up: it is loaded before the plugin receives any
    // events, so there is no stream start for it - the first "playing" checks it instead.
    void OnPlayerState(int state) {
        if (state != AIMP_PLAYER_STATE_PLAYING) return;
        if (player) ApplyPendingSeek(true);
        if (waitingForPlay_ || CurrentFile() != handledFile_) {
            waitingForPlay_ = false;
            OnStreamStart();
        }
    }

    // Main thread: checks the playing track again without counting it as a new track
    void RecheckSamePlayback() {
        handledFile_.clear();
        if (!player || player->GetState() != AIMP_PLAYER_STATE_PLAYING) return;  // done when playback starts
        restartFile_ = CurrentFile();
        OnStreamStart();
    }

    // Main thread: checks the playing track again (e.g. after the plugin was enabled). Anything may have changed
    // meanwhile (e.g. Voicemeeter's engine rate while the plugin was off): compared like after an output change.
    void Recheck() {
        handledFile_.clear();
        if (!player || player->GetState() != AIMP_PLAYER_STATE_PLAYING) return;
        pendingOutputChange_ = true;
        OnStreamStart();
        pendingOutputChange_ = false;
    }

    tstring CurrentFile() {
        Ptr<IAIMPFileInfo> info;
        Ptr<IAIMPString> s;
        if (!player || Failed(player->GetInfo(info.Out())) || !info) return tstring();
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, s.OutV())) && s)
            return FromString(s.Get());
        return tstring();
    }

    // Internet radio and other streams (http://, https://, mms:// ...): never switched. Switching stops and
    // restarts playback - for a live stream that means reconnecting, and with the legacy restart a whole
    // AIMP restart on every station change. A local file has no scheme (Windows "C:\...", Linux "/..."),
    // a file:// URL counts as local. AIMP's own check (IsURL) is asked as well.
    static bool IsStream(const tstring& file) {
        size_t colon = file.find(AR_T("://"));
        if (colon != tstring::npos && colon >= 2) {
            bool scheme = true;
            for (size_t i = 0; i < colon; i++) {
                TChar c = file[i];
                scheme = scheme && ((c >= AR_T('a') && c <= AR_T('z')) || (c >= AR_T('A') && c <= AR_T('Z')) ||
                                    (i > 0 && ((c >= AR_T('0') && c <= AR_T('9')) || c == AR_T('+') || c == AR_T('-') || c == AR_T('.'))));
            }
            if (scheme) return !EqualsI(file.substr(0, colon), AR_T("file"));
        }
        auto uri = Service<IAIMPServiceFileURI>(IID_IAIMPServiceFileURI);
        auto s = MakeString(file);
        return uri && s && uri->IsURL(s.Get()) == S_OK;
    }

    // "http://host:port/path?key" -> "http://host:port" (the log never shows paths or listening keys)
    static tstring StreamHost(const tstring& url) {
        size_t start = url.find(AR_T("://"));
        if (start == tstring::npos) return tstring(AR_T("?"));
        size_t end = url.find_first_of(AR_T("/?#"), start + 3);
        tstring host = url.substr(0, end);
        size_t at = host.find(AR_T('@'), start + 3);  // user:password@host
        if (at != tstring::npos) host = host.substr(0, start + 3) + host.substr(at + 1);
        return host;
    }

    // Main thread: AIMP started a new stream
    void OnStreamStart() {
        if (player) ApplyPendingSeek(false);
        if (!cfg.enabled || !player) return;
        // Only act while AIMP is really playing: switching restarts playback, which must never start
        // music that the user had paused or stopped.
        if (player->GetState() != AIMP_PLAYER_STATE_PLAYING) {
            waitingForPlay_ = true;
            return;
        }
        waitingForPlay_ = false;
        Ptr<IAIMPFileInfo> info;
        if (Failed(player->GetInfo(info.Out())) || !info) return;
        Job j;
        INT32 v = 0;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, &v)) && v > 0) j.track.rate = (uint32_t)v;
        if (!j.track.rate) return;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_BITDEPTH, &v)) && v > 0) j.track.bits = (uint32_t)v;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_CHANNELS, &v)) && v > 0) j.track.channels = (uint32_t)v;
        Ptr<IAIMPString> s;
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_CODEC, IID_IAIMPString, s.OutV())) && s)
            j.track.codec = FromString(s.Get());
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, s.OutV())) && s)
            j.file = FromString(s.Get());
        j.track.aimpOutput = AimpOutput();
        j.track.lossy = IsLossy(j.track.codec, j.file);
        bool recheck = !restartFile_.empty() && restartFile_ == j.file;  // the plugin checks the same track again
        if (j.file != handledFile_ && !recheck) outputRechecks_ = 0;  // a new track
        lastOutput_ = j.track.aimpOutput;
        j.outputChange = pendingOutputChange_;
        pendingOutputChange_ = false;
        if (IsStream(j.file)) {
            bool same = j.file == handledFile_ || recheck;  // e.g. playback paused and continued
            handledFile_ = j.file;
            handledSkipped_ = true;
            restartFile_.clear();
            ++gen_;  // a job for the track before is not switched any more
            stats.Update([&](Stats& st) {
                if (!same) {
                    st.tracks++;
                    st.skipped++;
                }
                st.trackRate = j.track.rate;
                st.trackBits = j.track.bits;
                st.trackChannels = j.track.channels;
                st.trackCodec = j.track.codec;
                st.aimpOutput = j.track.aimpOutput;
                st.note = AR_T("internet stream - not switched");
            });
            if (!same)
                Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | internet stream (") + StreamHost(j.file) +
                    AR_T(") - not switched"));
            if (onStatsChanged) onStatsChanged();
            return;
        }
        player->GetPlaylistItem(j.item.Out());
        // Exceptions (General tab): lossy formats, folders and playlists - the output stays as it is
        tstring why;
        if (cfg.skipLossy && j.track.lossy) why = AR_T("lossy format");
        else {
            tstring entry = MatchSkipList(cfg.skipList, j.file, PlaylistName(j.item.Get()));
            if (!entry.empty()) why = AR_T("exception list (") + entry + AR_T(")");
        }
        if (!why.empty()) {
            bool same = j.file == handledFile_ || recheck;
            handledFile_ = j.file;
            handledSkipped_ = true;
            restartFile_.clear();
            ++gen_;
            stats.Update([&](Stats& st) {
                if (!same) {
                    st.tracks++;
                    st.skipped++;
                    st.rates[j.track.rate]++;
                }
                st.trackRate = j.track.rate;
                st.trackBits = j.track.bits;
                st.trackChannels = j.track.channels;
                st.trackCodec = j.track.codec;
                st.aimpOutput = j.track.aimpOutput;
                st.trackFile = j.file;
                st.exception = why;
                st.note = AR_T("not switched: ") + why;
            });
            if (!same)
                Log(AR_T("Track ") + Khz(j.track.rate) + (j.track.codec.empty() ? tstring() : AR_T(" | ") + j.track.codec) +
                    AR_T(" | not switched: ") + why);
            if (onStatsChanged) onStatsChanged();
            return;
        }
        handledFile_ = j.file;
        handledSkipped_ = false;
        j.cfg = cfg;
        j.gen = ++gen_;
        j.selfRestart = !restartFile_.empty() && restartFile_ == j.file;
        restartFile_.clear();
        stats.Update([&](Stats& st) {
            if (!j.selfRestart) {
                st.tracks++;
                st.rates[j.track.rate]++;
            }
            st.trackRate = j.track.rate;
            st.trackBits = j.track.bits;
            st.trackChannels = j.track.channels;
            st.trackCodec = j.track.codec;
            st.aimpOutput = j.track.aimpOutput;
            st.trackFile = j.file;
            st.exception.clear();
        });
        std::lock_guard<std::mutex> l(m_);
        job_ = std::move(j);  // drop an older job that was not handled yet
        has_ = true;
        cv_.notify_one();
    }

    // Main thread: the playlist the item plays from ("" if none)
    static tstring PlaylistName(IAIMPPlaylistItem* item) {
        if (!item) return tstring();
        Ptr<IAIMPPlaylist> pl;
        Ptr<IAIMPPropertyList> props;
        Ptr<IAIMPString> s;
        if (Failed(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_PLAYLIST, IID_IAIMPPlaylist, pl.OutV())) || !pl ||
            Failed(pl->QueryInterface(IID_IAIMPPropertyList, props.OutV())) || !props ||
            Failed(props->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, s.OutV())) || !s)
            return tstring();
        return FromString(s.Get());
    }

    // ---- B5: an output changed while a track plays (main thread)

    // Once a second while AIMP plays (AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION): AIMP's output or its own rate,
    // Windows' devices (reported by DeviceWatcher) - something new is checked like a new track
    void Tick() {
        if (!cfg.enabled || !player || player->GetState() != AIMP_PLAYER_STATE_PLAYING) return;
        if (handledFile_.empty() || handledSkipped_ || CurrentFile() != handledFile_) return;  // not handled (yet)
        tstring out = AimpOutput();
        if (!lastOutput_.empty() && !out.empty() && out != lastOutput_) {
            OutputChanged(AR_T("AIMP output ") + out);
            return;
        }
#ifdef _WIN32
        if (livePending_ && !win::LiveRate::Running() && !win::LiveRate::PreferencesOpen()) {
            livePending_ = false;
            OutputChanged(AR_T("AIMP's preferences closed"));
            return;
        }
        if (win::LiveRate::Running()) return;  // the plugin changes AIMP's rate right now
#endif
        uint32_t live = aimpSync.LiveAimpRate();
        if (lastAimpRate_ && live && live != lastAimpRate_) {
            lastAimpRate_ = live;
            OutputChanged(AR_T("AIMP's output rate ") + Khz(live));
            return;
        }
        // Windows reports one change several times: wait until it is quiet for 0.7 s. Changes the plugin
        // made itself (while switching and up to 3 s after it) are not counted.
        int64_t at = winChangedAt_;
        if (!at || applying_ || NowMs() - at < 700) return;
        winChangedAt_ = 0;
        if (at - lastApplyEnd_ < 3000) return;
        int what = winWhat_;
        OutputChanged(what == 1 ? AR_T("Windows default device") : what == 2 ? AR_T("Windows default format")
                                                                           : AR_T("Windows audio device added or removed"));
    }

    // Checks the playing track again because an output changed (at most 3 times per track: a device that
    // keeps changing its format by itself must not start an endless loop)
    void OutputChanged(const tstring& what) {
        if (!cfg.enabled || !player || player->GetState() != AIMP_PLAYER_STATE_PLAYING) return;
        if (outputRechecks_ >= 3) {
            if (outputRechecks_++ == 3) Log(AR_T("Output changed again (") + what + AR_T(") - not checked again for this track"));
            return;
        }
        outputRechecks_++;
        Log(AR_T("Output changed (") + what + AR_T(") - checking the current track again"));
        pendingOutputChange_ = true;
        RecheckSamePlayback();
        pendingOutputChange_ = false;  // not playing: nothing was queued
    }

    // ---- F1: switches the plugin on or off (menu, hotkey, settings page); main thread
    void SetEnabled(bool on, const TChar* how) {
        if (cfg.enabled == on) return;
        cfg.enabled = on;
        cfg.Save();
        bitPerfect.Apply(cfg);
        Log(tstring(on ? AR_T("Plugin enabled") : AR_T("Plugin disabled")) + (how ? AR_T(" (") + tstring(how) + AR_T(")") : tstring()));
        if (on) Recheck();  // switch to the rate of the track that is playing now
        if (onEnabledChanged) onEnabledChanged();
    }

    // ---- Counters are kept in the settings file, section [Statistics] (main thread)
    void LoadStats() {
        Ini& i = Ini::Get();
        const tstring sec = AR_T("Statistics");
        auto rd = [&](const TChar* n) { return (long)i.Int(sec, n, 0); };
        tstring rates = i.Str(sec, AR_T("Rates"));
        stats.Update([&](Stats& s) {
            s.tracks = rd(AR_T("Tracks"));
            s.switches = rd(AR_T("Switches"));
            s.skipped = rd(AR_T("Skipped"));
            s.failures = rd(AR_T("Failures"));
            s.vmRestarts = rd(AR_T("VoicemeeterRestarts"));
            s.aimpRestarts = rd(AR_T("AimpRestarts"));
            s.aimpLive = rd(AR_T("AimpRateChanges"));
            s.switchMsTotal = i.Int(sec, AR_T("SwitchMsTotal"), 0);
            for (auto& part : Split(rates, AR_T(';'))) {
                size_t eq = part.find(AR_T('='));
                if (eq == tstring::npos) continue;
                try {
                    long hz = std::stol(part.substr(0, eq)), n = std::stol(part.substr(eq + 1));
                    if (hz > 0 && n > 0) s.rates[(uint32_t)hz] = n;
                } catch (...) {  // damaged entry - ignore it
                }
            }
        });
    }

    void SaveStats() {
        Ini& i = Ini::Get();
        const tstring sec = AR_T("Statistics");
        Stats s = stats.Snapshot();
        i.Set(sec, AR_T("Tracks"), (long long)s.tracks);
        i.Set(sec, AR_T("Switches"), (long long)s.switches);
        i.Set(sec, AR_T("Skipped"), (long long)s.skipped);
        i.Set(sec, AR_T("Failures"), (long long)s.failures);
        i.Set(sec, AR_T("VoicemeeterRestarts"), (long long)s.vmRestarts);
        i.Set(sec, AR_T("AimpRestarts"), (long long)s.aimpRestarts);
        i.Set(sec, AR_T("AimpRateChanges"), (long long)s.aimpLive);
        i.Set(sec, AR_T("SwitchMsTotal"), (long long)s.switchMsTotal);
        tstring rates;
        for (auto& r : s.rates) rates += Num(r.first) + AR_T("=") + Num(r.second) + AR_T(";");
        i.Set(sec, AR_T("Rates"), rates);
        i.Save();
    }
};

}  // namespace ar
