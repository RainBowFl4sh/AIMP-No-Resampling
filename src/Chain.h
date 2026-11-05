// Audio chain (U2): the way of the samples from the file to the device that is heard, step by step, and
// whether each step passes them on unchanged (bit-perfect). BuildChain only works on the collected facts
// (ChainInput), so the mock host can test every case; ChainWindow.h collects them in AIMP.
#pragma once
#include "Config.h"
#include "Lang.h"

namespace ar {

enum class ChainStatus { Ok, Info, Warn, Bad, Off };

// Symbol of a step (drawn in ChainWindow.h)
enum class ChainIcon { Track, Dsp, Volume, Output, System, Voicemeeter, Device, Result };

// One line of a step: a name in grey ("Sample rate") and its value; tone colours the value
// (Ok = normal text, Bad = red, Warn = amber, Info = grey note). Without a name the value takes the whole line.
struct ChainRow {
    tstring key, value;
    ChainStatus tone = ChainStatus::Ok;
};

struct ChainStage {
    ChainStatus status;
    tstring title, text;  // text: all rows as plain text ("name: value" per line)
    ChainIcon icon = ChainIcon::Track;
    std::vector<ChainRow> rows;
};

struct ChainInput {
    bool enabled = false, playing = false;
    // the file
    tstring file, codec, exception;
    uint32_t rate = 0, bits = 0, channels = 0, bitrate = 0;  // bitrate in kbit/s
    bool stream = false, lossy = false;
    // AIMP
    OutputMode mode = OutputMode::Shared;  // resolved output type
    tstring aimpOutput;
    uint32_t aimpRate = 0;                 // AIMP's own fixed rate (AIMPSoundOut\DeviceFreq), 0 = unknown
    std::vector<const char*> changes;      // AIMP features that change the signal (translation keys)
    double eqMaxDb = 0;                    // equalizer: largest band gain (dB, absolute)
    int volume = -1;                       // AIMP's volume in %, -1 = unknown
    bool crossfade = false;
    int crossfadeMs = 0, pauseMs = 0;      // crossfade length, pause between tracks (0 = gapless)
    // the plugin
    uint32_t deviceRate = 0;               // output rate after switching (statistics)
    uint32_t limitedTo = 0;                // limited by the highest rate setting
    // Windows: the device AIMP plays into (WASAPI shared, DirectSound), read when the window opens
    bool winKnown = false, winFloat = false, winMuted = false;
    tstring winDevice;
    uint32_t winRate = 0, winBits = 0;
    int winVolume = -1, enhancements = -1;
    // Voicemeeter
    bool vm = false;                       // AIMP plays into Voicemeeter
    bool vmInstalled = false;              // Voicemeeter is installed (shown as "not involved" otherwise)
    uint32_t vmRate = 0;
    // the device that is heard
    tstring endDevice;
    uint32_t endBits = 0;
};

inline tstring ChainFileName(const tstring& file) {
    size_t q = file.find_first_of(AR_T("?#"));
    tstring f = q == tstring::npos ? file : file.substr(0, q);
    while (!f.empty() && (f.back() == AR_T('/') || f.back() == AR_T('\\'))) f.pop_back();
    size_t sep = f.find_last_of(AR_T("\\/"));
    return sep == tstring::npos ? f : f.substr(sep + 1);
}

inline tstring JoinList(const std::vector<tstring>& v) {
    tstring r;
    for (auto& s : v) r += (r.empty() ? tstring() : tstring(AR_T(", "))) + s;
    return r;
}

inline bool ChainOnWindows() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

// The whole way: the steps, the result and one short line for the Output tab
struct ChainView {
    std::vector<ChainStage> stages;
    ChainStatus status = ChainStatus::Off;  // of the result
    tstring summary;
};

// What a step does to the signal: Bad = changes it, Warn = changes it a little / only sometimes
inline ChainStatus ChainWorst(const std::vector<ChainRow>& rows) {
    ChainStatus r = ChainStatus::Ok;
    for (auto& x : rows)
        if (x.tone == ChainStatus::Bad) r = ChainStatus::Bad;
        else if (x.tone == ChainStatus::Warn && r != ChainStatus::Bad) r = ChainStatus::Warn;
    return r;
}

inline tstring ChainOutputType(OutputMode m) {
    switch (m) {
        case OutputMode::Shared: return AR_T("WASAPI shared");
        case OutputMode::Exclusive: return AR_T("WASAPI exclusive");
        case OutputMode::Asio: return AR_T("ASIO");
        case OutputMode::DirectSound: return AR_T("DirectSound");
        case OutputMode::PipeWire: return AR_T("PipeWire");
        case OutputMode::Direct: return AR_T("ALSA");
        default: return tstring();
    }
}

inline tstring ChainDb(double db) {
    long long t = (long long)(db * 10 + (db >= 0 ? 0.5 : -0.5));
    tstring s = Num(t / 10) + AR_T(".") + Num((t < 0 ? -t : t) % 10);
    if (t < 0 && t > -10) s = AR_T("-") + s;
    return s + AR_T(" dB");
}

inline ChainView BuildChain(const ChainInput& in) {
    ChainView view;
    std::vector<ChainStage>& st = view.stages;
    // short reasons for the result: resampling (red), other changes of the samples (yellow: at least nothing is
    // resampled), hints (yellow)
    std::vector<tstring> resample, bad, warn;
    auto add = [&](ChainIcon icon, const char* title, std::vector<ChainRow> rows, ChainStatus status) {
        ChainStage s;
        s.icon = icon;
        s.title = Tr(title);
        s.rows = std::move(rows);
        s.status = status;
        for (auto& r : s.rows) s.text += (s.text.empty() ? tstring() : tstring(AR_NL)) + (r.key.empty() ? r.value : r.key + AR_T(": ") + r.value);
        st.push_back(s);
    };
    auto row = [](const char* key, const tstring& value, ChainStatus tone = ChainStatus::Ok) {
        return ChainRow{key ? Tr(key) : tstring(), value, tone};
    };
    const ChainStatus OK = ChainStatus::Ok, INFO = ChainStatus::Info, WARN = ChainStatus::Warn, BAD = ChainStatus::Bad;

    // --- the track
    {
        std::vector<ChainRow> r;
        ChainStatus s = OK;
        if (!in.playing) {
            r.push_back(row(nullptr, Tr("Nothing is playing."), INFO));
            s = ChainStatus::Off;
        } else {
            r.push_back(row(nullptr, ChainFileName(in.file)));
            tstring fmt = in.codec;
            if (in.channels) fmt += (fmt.empty() ? tstring() : tstring(AR_T(" · "))) + Trf("%1 ch", {Num(in.channels)});
            if (in.bitrate) fmt += (fmt.empty() ? tstring() : tstring(AR_T(" · "))) + Num(in.bitrate) + AR_T(" kbps");
            if (!fmt.empty()) r.push_back(row("Format", fmt));
            r.push_back(row("Sample rate", Khz(in.rate)));
            if (in.lossy) r.push_back(row("Bit depth", Tr("none (lossy format)")));
            else if (in.bits) r.push_back(row("Bit depth", Num(in.bits) + AR_T(" bit")));
            if (in.stream) {
                s = INFO;
                r.push_back(row(nullptr, Tr("Internet stream: never switched (the station decides the rate)."), INFO));
            } else if (!in.exception.empty()) {
                s = INFO;
                r.push_back(row(nullptr, Tr("Exception (General tab): the plugin does not switch for this track."), INFO));
            } else if (in.lossy) {
                s = INFO;
                r.push_back(row(nullptr, Tr("Lossy format: AIMP decodes it; after that nothing more needs to be lost."), INFO));
            }
        }
        add(ChainIcon::Track, "Track", r, s);
    }

    // --- AIMP: sound processing (DSP) and the rate it sends out
    bool shared = in.mode == OutputMode::Shared;
    bool fixed = !shared;  // AIMP plays with its own fixed rate (exclusive, ASIO, DirectSound, Linux)
    uint32_t out = !in.playing ? 0 : shared ? (in.winKnown ? in.winRate : in.deviceRate) : (in.aimpRate ? in.aimpRate : in.deviceRate);
    // reasons for the result, in this order: AIMP's effects, its volume (resampling is listed first anyway)
    std::vector<tstring> aimpBad, aimpWarn;
    for (const char* c : in.changes) aimpBad.push_back(Tr(c));
    if (in.volume >= 0 && in.volume < 100) aimpBad.push_back(Trf("volume %1 %", {Num(in.volume)}));
    if (in.crossfade) aimpWarn.push_back(Tr("crossfade at track changes"));
    {
        std::vector<ChainRow> r;
        std::vector<tstring> eq, fx, level;
        for (const char* c : in.changes) {
            std::string k = c;
            if (k == "equalizer") eq.push_back(Tr(c));
            else if (k == "ReplayGain" || k == "peak normalization" || k == "loudness compensation") level.push_back(Tr(c));
            else fx.push_back(Tr(c));
        }
        if (eq.empty()) r.push_back(row("Equalizer", Tr("off")));
        else r.push_back(row("Equalizer", in.eqMaxDb >= 0.05 ? Trf("on (bands up to %1)", {ChainDb(in.eqMaxDb)}) : Tr("on"), BAD));
        r.push_back(row("Sound effects", fx.empty() ? Tr("off") : JoinList(fx), fx.empty() ? OK : BAD));
        r.push_back(row("Volume levelling", level.empty() ? Tr("off") : JoinList(level), level.empty() ? OK : BAD));
        if (in.crossfade)
            r.push_back(row("Crossfade", in.crossfadeMs > 0 ? Trf("%1 s at track changes", {Num((in.crossfadeMs + 500) / 1000)})
                                                           : Tr("at track changes"), WARN));
        else
            r.push_back(row("Crossfade", Tr("off")));
        if (in.pauseMs > 0) r.push_back(row("Gapless", Trf("off (%1 ms pause between tracks)", {Num(in.pauseMs)}), INFO));
        // resampling inside AIMP
        if (out && in.rate && out != in.rate) {
            tstring rr = Khz(in.rate) + AR_T(" → ") + Khz(out);
            tstring why = Trf("resamples %1 → %2", {Khz(in.rate), Khz(out)});
            if (in.limitedTo && out == in.limitedTo) {
                r.push_back(row("Resampling", rr + AR_T(" (") + Tr("highest rate setting") + AR_T(")"), BAD));
                resample.push_back(AR_T("AIMP ") + why + AR_T(" (") + Tr("highest rate setting") + AR_T(")"));
            } else if (out > in.rate && out % in.rate == 0) {
                r.push_back(row("Resampling", rr + AR_T(" (") + Tr("integer multiple") + AR_T(")"), BAD));
                resample.push_back(AR_T("AIMP ") + why + AR_T(" (") + Tr("integer multiple") + AR_T(")"));
            } else {
                r.push_back(row("Resampling", rr, BAD));
                resample.push_back(AR_T("AIMP ") + why);
            }
        } else {
            r.push_back(row("Resampling", out ? Trf("none (%1)", {Khz(out)}) : Tr("none")));
        }
        add(ChainIcon::Dsp, "AIMP sound processing", r, ChainWorst(r));
    }

    // --- AIMP's volume
    {
        std::vector<ChainRow> r;
        if (in.volume < 0) {
            r.push_back(row("Volume", Tr("not known"), INFO));
        } else if (in.volume < 100) {
            r.push_back(row("Volume", Num(in.volume) + AR_T(" % (") + Tr("changes the samples") + AR_T(")"), BAD));
        } else {
            r.push_back(row("Volume", Tr("100 % (unchanged)")));
        }
        add(ChainIcon::Volume, "AIMP volume", r, ChainWorst(r));
    }
    for (auto& b : aimpBad) bad.push_back(AR_T("AIMP ") + b);
    for (auto& w : aimpWarn) warn.push_back(AR_T("AIMP ") + w);

    // --- AIMP's output
    {
        std::vector<ChainRow> r;
        r.push_back(row(nullptr, in.aimpOutput.empty() ? Tr("Output not known.") : in.aimpOutput, in.aimpOutput.empty() ? INFO : OK));
        tstring type = ChainOutputType(in.mode);
        if (!type.empty()) r.push_back(row("Type", type));
        if (out) {
            tstring how;
            if (fixed && in.aimpRate) how = Tr("fixed in AIMP");
            else if (in.enabled && in.deviceRate == out) how = Tr("set by the plugin");
            r.push_back(row("Sample rate", Khz(out) + (how.empty() ? tstring() : AR_T(" (") + how + AR_T(")"))));
        }
        r.push_back(row("Prevent Resampling", in.enabled ? Tr("on") : Tr("off (rates are not switched)"), in.enabled ? OK : INFO));
        add(ChainIcon::Output, "AIMP output", r, in.aimpOutput.empty() ? INFO : OK);
    }

    // --- between AIMP and the device
    uint32_t endRate = out;
    if (ChainOnWindows() && (shared || in.mode == OutputMode::DirectSound)) {
        std::vector<ChainRow> r;
        std::vector<tstring> issues, hints;
        if (!in.winKnown) {
            r.push_back(row(nullptr, Tr("The device's default format could not be read."), INFO));
        } else {
            r.push_back(row(nullptr, in.winDevice));
            r.push_back(row("Default format", Khz(in.winRate) + AR_T(" · ") + Num(in.winBits) + (in.winFloat ? AR_T(" bit float") : AR_T(" bit"))));
            if (in.playing && in.mode == OutputMode::DirectSound && in.aimpRate && in.aimpRate != in.winRate) {
                resample.push_back(AR_T("Windows ") + Trf("resamples %1 → %2", {Khz(in.aimpRate), Khz(in.winRate)}));
                r.push_back(row("Resampling", Khz(in.aimpRate) + AR_T(" → ") + Khz(in.winRate), BAD));
            }
            bool cut = in.playing && !in.lossy && in.bits && in.winBits && !in.winFloat && in.winBits < in.bits;
            if (cut) issues.push_back(Trf("reduces %1 → %2 bit", {Num(in.bits), Num(in.winBits)}));
            r.push_back(row("Mixing", cut ? Trf("reduces %1 → %2 bit", {Num(in.bits), Num(in.winBits)})
                                          : Tr("32-bit float, samples unchanged"), cut ? BAD : OK));
            if (in.winVolume >= 0 && in.winVolume < 100) {
                issues.push_back(Trf("volume %1 %", {Num(in.winVolume)}));
                r.push_back(row("Windows volume", Num(in.winVolume) + AR_T(" % (") + Tr("changes the samples") + AR_T(")"), BAD));
            } else if (in.winVolume >= 0) {
                r.push_back(row("Windows volume", Tr("100 % (unchanged)")));
            }
            if (in.winMuted) {
                hints.push_back(Tr("muted"));
                r.push_back(row("Windows volume", Tr("muted"), WARN));
            }
            if (in.enhancements == 1) {
                hints.push_back(Tr("audio enhancements may be on"));
                r.push_back(row("Enhancements", Tr("may be on"), WARN));
            } else if (in.enhancements == 0) {
                r.push_back(row("Enhancements", Tr("off")));
            }
            endRate = in.winRate;
        }
        for (auto& b : issues) bad.push_back(AR_T("Windows ") + b);
        for (auto& h : hints) warn.push_back(AR_T("Windows: ") + h);
        add(ChainIcon::System, "Windows", r, !in.winKnown ? INFO : ChainWorst(r));
    } else if (ChainOnWindows()) {
        add(ChainIcon::System, "Windows", {row(nullptr, Tr("Not involved: AIMP opens the device itself (exclusive / ASIO)."))}, OK);
    } else if (in.mode == OutputMode::PipeWire) {
        std::vector<ChainRow> r;
        if (in.deviceRate) r.push_back(row("Graph rate", Khz(in.deviceRate) + AR_T(" (clock.force-rate)")));
        if (in.playing && in.deviceRate && out && in.deviceRate != out) {
            tstring rr = Trf("resamples %1 → %2", {Khz(out), Khz(in.deviceRate)});
            resample.push_back(AR_T("PipeWire ") + rr);
            r.push_back(row("Resampling", Khz(out) + AR_T(" → ") + Khz(in.deviceRate), BAD));
        } else {
            r.push_back(row(nullptr, in.deviceRate ? Trf("Runs at %1 like AIMP - no second resampling.", {Khz(in.deviceRate)})
                                                   : Tr("Rate not set yet."), in.deviceRate ? OK : INFO));
        }
        if (in.deviceRate) endRate = in.deviceRate;
        add(ChainIcon::System, "PipeWire", r, ChainWorst(r));
    } else {
        add(ChainIcon::System, "ALSA", {row(nullptr, Tr("ALSA direct: AIMP opens the device itself."))}, OK);
    }

    if (in.vm) {
        uint32_t into = endRate;
        std::vector<ChainRow> r;
        r.push_back(row("Engine rate", in.vmRate ? Khz(in.vmRate) : Tr("not known"), in.vmRate ? OK : INFO));
        if (in.vmRate && into && in.vmRate != into) {
            resample.push_back(AR_T("Voicemeeter ") + Trf("resamples %1 → %2", {Khz(into), Khz(in.vmRate)}));
            r.push_back(row("Resampling", Khz(into) + AR_T(" → ") + Khz(in.vmRate), BAD));
        }
        r.push_back(row(nullptr, Tr("Leave bus EQ and effects off, fader at 0 dB (not checked)."), INFO));
        if (in.vmRate) endRate = in.vmRate;
        add(ChainIcon::Voicemeeter, "Voicemeeter", r, ChainWorst(r));
    } else if (in.vmInstalled) {
        add(ChainIcon::Voicemeeter, "Voicemeeter", {row(nullptr, Tr("Not involved: AIMP does not play into Voicemeeter."))}, OK);
    }

    // --- the device that is heard
    {
        std::vector<ChainRow> r;
        r.push_back(row(nullptr, in.endDevice.empty() ? Tr("unknown device") : in.endDevice, in.endDevice.empty() ? INFO : OK));
        if (endRate) r.push_back(row("Sample rate", Khz(endRate)));
        if (in.endBits) r.push_back(row("Bit depth", Num(in.endBits) + AR_T(" bit")));
        add(ChainIcon::Device, "Device", r, endRate ? OK : INFO);
    }

    // --- result
    tstring name = in.endDevice.empty() ? Tr("the device") : in.endDevice;
    auto first = [](const std::vector<tstring>& v) { return v.front() + (v.size() > 1 ? AR_T(" …") : AR_T("")); };
    tstring result;
    if (!resample.empty()) {  // red: resampled somewhere
        std::vector<tstring> all = resample;
        all.insert(all.end(), bad.begin(), bad.end());
        view.status = BAD;
        result = Trf("Not bit-perfect: %1.", {JoinList(all)});
        view.summary = Trf("Not bit-perfect: %1", {first(all)});
    } else if (!bad.empty()) {  // yellow: the samples are changed, but at least nothing is resampled
        view.status = WARN;
        result = Trf("No resampling, but not bit-perfect: %1.", {JoinList(bad)});
        view.summary = Trf("No resampling, but not bit-perfect: %1", {first(bad)});
    } else if (!warn.empty()) {
        view.status = WARN;
        result = Trf("Almost bit-perfect: %1.", {JoinList(warn)});
        view.summary = Trf("Almost bit-perfect: %1", {first(warn)});
    } else if (in.playing && !endRate) {
        view.status = INFO;
        result = Tr("Not known yet: the rate behind AIMP could not be read.");
        view.summary = result;
    } else if (!in.playing) {
        view.status = ChainStatus::Off;
        result = Tr("Start a track to see its way to your device.");
        view.summary = Tr("Nothing is playing - AIMP itself does not change the sound.");
    } else if (in.lossy || in.stream) {
        view.status = OK;
        result = Trf("Bit-perfect after decoding: the samples reach %1 unchanged.", {name});
        view.summary = Tr("Bit-perfect after decoding");
    } else {
        view.status = OK;
        result = Trf("Bit-perfect: the samples of the file reach %1 unchanged.", {name});
        view.summary = Tr("Bit-perfect");
    }
    std::vector<ChainRow> rr = {row(nullptr, result, view.status == ChainStatus::Off ? INFO : OK)};
    if (!in.enabled && in.playing) rr.push_back(row(nullptr, Tr("The plugin is disabled (General tab): rates are not switched."), INFO));
    add(ChainIcon::Result, "Result", rr, view.status);
    return view;
}

}  // namespace ar
