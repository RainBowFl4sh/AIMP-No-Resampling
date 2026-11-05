// Audio chain window (U2): shows the way from the file to the device that is heard (Chain.h) with a status
// sign per step. Opened from the Output tab, AIMP's menu or a hotkey; a modal AIMP window that follows the skin.
// Everything runs on AIMP's main thread.
#pragma once
#include <memory>

#include "AvatarFx.h"  // GDI+ start-up (Windows)
#include "Chain.h"
#include "Engine.h"
#include "UiCommon.h"

namespace ar {

// ---------------------------------------------------------------- status signs
// A coloured circle with a white sign; colours that read on light and dark skins alike
inline uint32_t ChainColor(ChainStatus s) {
    switch (s) {
        case ChainStatus::Ok: return 0x2EA043;
        case ChainStatus::Info: return 0x3B82F6;
        case ChainStatus::Warn: return 0xD29922;
        case ChainStatus::Bad: return 0xD73A49;
        case ChainStatus::Off: return 0x8B949E;
    }
    return 0x8B949E;
}

// Lines of the sign in a circle of radius 1 around (0, 0): {x0, y0, x1, y1}, a dot is a line of length 0
struct ChainLine { float x0, y0, x1, y1; };
inline std::vector<ChainLine> ChainSign(ChainStatus s) {
    switch (s) {
        case ChainStatus::Ok: return {{-0.45f, 0.02f, -0.12f, 0.36f}, {-0.12f, 0.36f, 0.48f, -0.34f}};
        case ChainStatus::Bad: return {{-0.36f, -0.36f, 0.36f, 0.36f}, {0.36f, -0.36f, -0.36f, 0.36f}};
        case ChainStatus::Warn: return {{0, -0.5f, 0, 0.12f}, {0, 0.46f, 0, 0.46f}};
        case ChainStatus::Info: return {{0, -0.46f, 0, -0.46f}, {0, -0.12f, 0, 0.5f}};
        case ChainStatus::Off: return {{-0.4f, 0, 0.4f, 0}};
    }
    return {};
}

// Symbol of a step in a circle of radius 1 around (0, 0), drawn in white on the step's colour
struct ChainGlyph {
    enum Kind { Line, Dot, Ring, ArcTop, ArcRight, Trap } kind;
    float x0, y0, x1, y1;  // Line: from - to; Dot / Ring / ArcTop / ArcRight: centre (x0, y0), radius x1;
                           // Trap: filled, from x0 (half height y0) to x1 (half height y1) around y = 0
    float width;           // stroke width relative to the normal one
};
inline std::vector<ChainGlyph> ChainIconGlyphs(ChainIcon icon, ChainStatus status) {
    using G = ChainGlyph;
    switch (icon) {
        case ChainIcon::Track:  // a note
            return {{G::Dot, -0.17f, 0.30f, 0.21f, 0, 1}, {G::Line, 0.03f, 0.30f, 0.03f, -0.46f, 1},
                    {G::Line, 0.03f, -0.46f, 0.38f, -0.27f, 1}};
        case ChainIcon::Dsp:  // equalizer sliders
            return {{G::Line, -0.38f, -0.48f, -0.38f, 0.48f, 0.6f}, {G::Line, 0, -0.48f, 0, 0.48f, 0.6f},
                    {G::Line, 0.38f, -0.48f, 0.38f, 0.48f, 0.6f}, {G::Dot, -0.38f, 0.16f, 0.14f, 0, 1},
                    {G::Dot, 0, -0.22f, 0.14f, 0, 1}, {G::Dot, 0.38f, 0.04f, 0.14f, 0, 1}};
        case ChainIcon::Volume:  // a knob
            return {{G::Ring, 0, 0, 0.48f, 0, 0.9f}, {G::Line, 0, 0, 0.22f, -0.30f, 1}};
        case ChainIcon::Output:  // out of AIMP
            return {{G::Line, -0.46f, 0, 0.42f, 0, 1}, {G::Line, 0.14f, -0.28f, 0.44f, 0, 1},
                    {G::Line, 0.14f, 0.28f, 0.44f, 0, 1}, {G::Line, -0.46f, -0.34f, -0.46f, 0.34f, 1}};
        case ChainIcon::System:  // a screen
            return {{G::Line, -0.48f, -0.40f, 0.48f, -0.40f, 0.8f}, {G::Line, 0.48f, -0.40f, 0.48f, 0.18f, 0.8f},
                    {G::Line, 0.48f, 0.18f, -0.48f, 0.18f, 0.8f}, {G::Line, -0.48f, 0.18f, -0.48f, -0.40f, 0.8f},
                    {G::Line, 0, 0.18f, 0, 0.42f, 0.8f}, {G::Line, -0.24f, 0.44f, 0.24f, 0.44f, 0.8f}};
        case ChainIcon::Voicemeeter:  // a mixer
            return {{G::Line, -0.48f, -0.34f, 0.48f, -0.34f, 0.6f}, {G::Line, -0.48f, 0, 0.48f, 0, 0.6f},
                    {G::Line, -0.48f, 0.34f, 0.48f, 0.34f, 0.6f}, {G::Dot, 0.20f, -0.34f, 0.14f, 0, 1},
                    {G::Dot, -0.24f, 0, 0.14f, 0, 1}, {G::Dot, 0.08f, 0.34f, 0.14f, 0, 1}};
        case ChainIcon::Device:  // a loudspeaker with sound waves (headphones, speakers, DAC: any device)
            return {{G::Trap, -0.58f, 0.16f, -0.34f, 0.16f, 1}, {G::Trap, -0.36f, 0.16f, -0.08f, 0.44f, 1},
                    {G::ArcRight, -0.06f, 0, 0.28f, 0, 0.7f}, {G::ArcRight, -0.06f, 0, 0.54f, 0, 0.7f}};
        case ChainIcon::Result: {
            std::vector<ChainGlyph> r;
            for (auto& l : ChainSign(status))
                r.push_back(l.x0 == l.x1 && l.y0 == l.y1 ? ChainGlyph{G::Dot, l.x0, l.y0, 0.12f, 0, 1}
                                                         : ChainGlyph{G::Line, l.x0, l.y0, l.x1, l.y1, 1});
            return r;
        }
    }
    return {};
}

struct ChainMark {
    float cy;          // centre in pixels
    ChainStatus status;
    std::vector<ChainGlyph> glyphs;
};

// Draws the marks at (cx, cy) with radius r, joined by arrows (the chain) when arrows is set
inline void DrawChainMarks(HCANVAS canvas, const std::vector<ChainMark>& marks, float cx, float r, bool arrows) {
    if (!canvas || marks.empty()) return;
    const float line = (std::max)(1.0f, r * 0.12f), stroke = (std::max)(1.5f, r * 0.16f);
    const float head = (std::max)(3.0f, r * 0.35f);
#ifdef _WIN32
    if (!fx::EnsureGdip()) return;
    Gdiplus::Graphics g(canvas);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    auto col = [](uint32_t rgb) { return Gdiplus::Color(255, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb); };
    if (arrows) {
        Gdiplus::Pen rail(col(0x9AA0A6), line);
        rail.SetStartCap(Gdiplus::LineCapRound);
        rail.SetEndCap(Gdiplus::LineCapRound);
        for (size_t i = 0; i + 1 < marks.size(); i++) {
            float y0 = marks[i].cy + r + 3, y1 = marks[i + 1].cy - r - 3;
            if (y1 - y0 < head) continue;
            g.DrawLine(&rail, cx, y0, cx, y1);
            g.DrawLine(&rail, cx - head * 0.7f, y1 - head, cx, y1);
            g.DrawLine(&rail, cx + head * 0.7f, y1 - head, cx, y1);
        }
    }
    for (auto& m : marks) {
        Gdiplus::SolidBrush b(col(ChainColor(m.status)));
        g.FillEllipse(&b, cx - r, m.cy - r, 2 * r, 2 * r);
        Gdiplus::SolidBrush w(col(0xFFFFFF));
        for (auto& gl : m.glyphs) {
            Gdiplus::Pen pen(col(0xFFFFFF), stroke * gl.width);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            float x = cx + gl.x0 * r, y = m.cy + gl.y0 * r, rr = gl.x1 * r;
            switch (gl.kind) {
                case ChainGlyph::Line: g.DrawLine(&pen, x, y, cx + gl.x1 * r, m.cy + gl.y1 * r); break;
                case ChainGlyph::Dot: g.FillEllipse(&w, x - rr, y - rr, 2 * rr, 2 * rr); break;
                case ChainGlyph::Ring: g.DrawEllipse(&pen, x - rr, y - rr, 2 * rr, 2 * rr); break;
                case ChainGlyph::ArcTop: g.DrawArc(&pen, x - rr, y - rr, 2 * rr, 2 * rr, 180.0f, 180.0f); break;
                case ChainGlyph::ArcRight: g.DrawArc(&pen, x - rr, y - rr, 2 * rr, 2 * rr, -50.0f, 100.0f); break;
                case ChainGlyph::Trap: {
                    Gdiplus::PointF pts[4] = {{x, m.cy - gl.y0 * r}, {cx + gl.x1 * r, m.cy - gl.y1 * r},
                                              {cx + gl.x1 * r, m.cy + gl.y1 * r}, {x, m.cy + gl.y0 * r}};
                    g.FillPolygon(&w, pts, 4);
                    break;
                }
            }
        }
    }
#else
    cairo_t* cr = canvas;
    cairo_save(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
    auto src = [&](uint32_t rgb) {
        cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
    };
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    if (arrows) {
        cairo_set_line_width(cr, line);
        src(0x9AA0A6);
        for (size_t i = 0; i + 1 < marks.size(); i++) {
            float y0 = marks[i].cy + r + 3, y1 = marks[i + 1].cy - r - 3;
            if (y1 - y0 < head) continue;
            cairo_move_to(cr, cx, y0);
            cairo_line_to(cr, cx, y1);
            cairo_move_to(cr, cx - head * 0.7f, y1 - head);
            cairo_line_to(cr, cx, y1);
            cairo_line_to(cr, cx + head * 0.7f, y1 - head);
            cairo_stroke(cr);
        }
    }
    for (auto& m : marks) {
        src(ChainColor(m.status));
        cairo_new_path(cr);
        cairo_arc(cr, cx, m.cy, r, 0, 2 * fx::kPi);
        cairo_fill(cr);
        src(0xFFFFFF);
        for (auto& gl : m.glyphs) {
            cairo_new_path(cr);
            cairo_set_line_width(cr, stroke * gl.width);
            float x = cx + gl.x0 * r, y = m.cy + gl.y0 * r, rr = gl.x1 * r;
            switch (gl.kind) {
                case ChainGlyph::Line: cairo_move_to(cr, x, y); cairo_line_to(cr, cx + gl.x1 * r, m.cy + gl.y1 * r); cairo_stroke(cr); break;
                case ChainGlyph::Dot: cairo_arc(cr, x, y, rr, 0, 2 * fx::kPi); cairo_fill(cr); break;
                case ChainGlyph::Ring: cairo_arc(cr, x, y, rr, 0, 2 * fx::kPi); cairo_stroke(cr); break;
                case ChainGlyph::ArcTop: cairo_arc(cr, x, y, rr, fx::kPi, 2 * fx::kPi); cairo_stroke(cr); break;
                case ChainGlyph::ArcRight: cairo_arc(cr, x, y, rr, -fx::kPi * 50 / 180, fx::kPi * 50 / 180); cairo_stroke(cr); break;
                case ChainGlyph::Trap:
                    cairo_move_to(cr, x, m.cy - gl.y0 * r);
                    cairo_line_to(cr, cx + gl.x1 * r, m.cy - gl.y1 * r);
                    cairo_line_to(cr, cx + gl.x1 * r, m.cy + gl.y1 * r);
                    cairo_line_to(cr, x, m.cy + gl.y0 * r);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                    break;
            }
        }
    }
    cairo_restore(cr);
#endif
}

// One status sign that fills the rectangle (the big sign above the chain)
inline void DrawChainStatus(HCANVAS canvas, const RECT& r, ChainStatus s) {
    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    float rad = ((std::min)(w, h) - 2) / 2;
    if (rad < 2) return;
    DrawChainMarks(canvas, {{r.top + h / 2, s, ChainIconGlyphs(ChainIcon::Result, s)}}, r.left + w / 2, rad, false);
}

// Text in an exact colour. Coloured values are drawn by the plugin itself: AIMP tints the text colour of its own
// labels with the skin's colour scheme (accent colour, hue), which turned red and yellow into the accent colour.
// lineH is the height of a line in pixels; long texts wrap at spaces.
inline void DrawChainText(HCANVAS canvas, const RECT& r, const tstring& text, uint32_t rgb, float lineH) {
    if (!canvas || text.empty() || lineH < 2) return;
    float width = (float)(r.right - r.left), px = lineH * 0.75f;  // 12 px font in a 16 px line (96 DPI)
    (void)px;
    auto wrap = [&](std::function<float(const tstring&)> measure) {
        std::vector<tstring> lines;
        tstring line;
        size_t i = 0;
        while (i <= text.size()) {
            size_t sp = text.find_first_of(AR_T(" \n"), i);
            if (sp == tstring::npos) sp = text.size();
            tstring word = text.substr(i, sp - i);
            tstring tryLine = line.empty() ? word : line + AR_T(" ") + word;
            if (!line.empty() && measure(tryLine) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = tryLine;
            }
            if (sp < text.size() && text[sp] == AR_T('\n')) {
                lines.push_back(line);
                line.clear();
            }
            i = sp + 1;
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    };
#ifdef _WIN32
    if (!fx::EnsureGdip()) return;
    Gdiplus::Graphics g(canvas);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    // the font AIMP selected into the canvas (the one of its labels); the system's message font otherwise
    std::unique_ptr<Gdiplus::Font> font(new Gdiplus::Font(canvas));
    if (font->GetLastStatus() != Gdiplus::Ok || font->GetHeight(96.0f) < 4) {
        NONCLIENTMETRICSW ncm;
        memset(&ncm, 0, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        const wchar_t* face = L"Segoe UI";
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) && ncm.lfMessageFont.lfFaceName[0])
            face = ncm.lfMessageFont.lfFaceName;
        Gdiplus::FontFamily family(face);
        font.reset(new Gdiplus::Font(family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif(), px,
                                     Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    }
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb));
    auto lines = wrap([&](const tstring& t) {
        Gdiplus::RectF box;
        g.MeasureString(t.c_str(), (INT)t.size(), font.get(), Gdiplus::PointF(0, 0), &fmt, &box);
        return box.Width;
    });
    float fh = font->GetHeight(&g);
    float y = (float)r.top + (std::max)(0.0f, (lineH - fh) / 2);
    for (auto& l : lines) {
        g.DrawString(l.c_str(), (INT)l.size(), font.get(), Gdiplus::PointF((float)r.left, y), &fmt, &brush);
        y += lineH;
    }
#else
    cairo_t* cr = canvas;
    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, lineH * 0.69f);  // the size of AIMP's labels on Linux
    cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
    auto lines = wrap([&](const tstring& t) {
        cairo_text_extents_t e;
        cairo_text_extents(cr, t.c_str(), &e);
        return (float)e.x_advance;
    });
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    float y = (float)r.top + (lineH - (float)(fe.ascent + fe.descent)) / 2 + (float)fe.ascent - 1;
    for (auto& l : lines) {
        cairo_move_to(cr, r.left, y);
        cairo_show_text(cr, l.c_str());
        y += lineH;
    }
    cairo_restore(cr);
#endif
}

// ---------------------------------------------------------------- the facts, collected in AIMP
inline ChainInput GatherChain(Engine& e) {
    ChainInput in;
    in.enabled = e.cfg.enabled;
    Stats s = e.stats.Snapshot();
    int state = e.player ? e.player->GetState() : AIMP_PLAYER_STATE_STOPPED;
    Ptr<IAIMPFileInfo> info;
    if (state != AIMP_PLAYER_STATE_STOPPED && e.player && Succeeded(e.player->GetInfo(info.Out())) && info) {
        INT32 v = 0;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, &v)) && v > 0) in.rate = (uint32_t)v;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_BITDEPTH, &v)) && v > 0) in.bits = (uint32_t)v;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_CHANNELS, &v)) && v > 0) in.channels = (uint32_t)v;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_BITRATE, &v)) && v > 0) in.bitrate = (uint32_t)v;
        Ptr<IAIMPString> str;
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_CODEC, IID_IAIMPString, str.OutV())) && str)
            in.codec = FromString(str.Get());
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, str.OutV())) && str)
            in.file = FromString(str.Get());
    }
    in.playing = in.rate > 0;
    in.stream = !in.file.empty() && Engine::IsStream(in.file);
    in.lossy = IsLossy(in.codec, in.file);
    in.aimpOutput = e.AimpOutput();
    bool current = !s.trackFile.empty() && s.trackFile == in.file;  // the statistics belong to this track
    in.exception = current ? s.exception : tstring();
    in.mode = ResolveMode(e.cfg.mode, in.aimpOutput);
    in.aimpRate = e.aimpSync.LiveAimpRate();
    if (!in.aimpRate) in.aimpRate = e.aimpSync.AimpRate();
    BitPerfect::Signal sig = BitPerfect::SignalState();
    in.changes = sig.changes;
    in.volume = sig.volume;
    in.crossfade = sig.crossfade;
    in.crossfadeMs = sig.crossfadeMs;
    in.pauseMs = sig.pauseMs;
    in.eqMaxDb = sig.eqMaxDb;
    if (current) {
        in.deviceRate = s.deviceRate;
        in.limitedTo = s.limitedTo;
        in.vm = s.vmChain;
        in.vmRate = s.vmEngineRate;
        in.endDevice = s.endDevice;
    }
#ifdef _WIN32
    in.vmInstalled = win::Voicemeeter::Get().Installed();
    // The Windows device AIMP plays into, looked up like the plugin does (the user may have changed it)
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (in.mode == OutputMode::Shared || in.mode == OutputMode::DirectSound) {
        auto all = win::ListRender();
        win::Endpoint dev;
        bool found = !e.cfg.windowsDevice.empty() ? win::FindByName(all, e.cfg.windowsDevice, dev)
                                                  : (win::FindInText(all, in.aimpOutput, dev) || win::DefaultRender(dev));
        if (found) {
            win::DeviceState ds = win::ReadDeviceState(dev.id);
            in.winDevice = dev.name;
            in.winKnown = ds.formatKnown;
            if (ds.formatKnown) {
                in.winRate = ds.format.Format.nSamplesPerSec;
                in.winBits = win::ValidBits(ds.format);
                in.winFloat = win::IsFloat(ds.format);
            }
            in.winVolume = ds.volume;
            in.winMuted = ds.muted;
            in.enhancements = ds.enhancements;
            if (!in.vm) {
                in.endDevice = dev.name;
                if (ds.formatKnown && !in.winFloat) in.endBits = in.winBits;
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
#endif
    if (in.endDevice.empty() && !in.vm) in.endDevice = s.endDevice;
    return in;
}

// ---------------------------------------------------------------- the chain on screen
// The bit-perfect status (big sign and one line, centred) and below it every step with its symbol, name and
// details. The same panel is used on the Output tab (in its scroll box) and in the audio chain window, so
// both look exactly alike. Labels are kept in pools and only moved, shown or hidden on a refresh.
class ChainPanel {
public:
    // design units (96 DPI; AIMP scales them)
    static const int kStatusH = 54;    // sign (28) + gap + summary line (18) + gap
    static const int kIcon = 24;       // symbol of a step
    static const int kText = 34;       // x of the step's name and details
    static const int kKeyW = 128;      // width of the grey names of the details
    static const int kLine = 16;       // height of a line of text
    static const int kGap = 16;        // between two steps (room for the arrow)
    static constexpr double kCharW = 6.0;  // average character width used to estimate line breaks

    // colours of the texts (0xRRGGBB): neutral grey (an AIMP label; grey is not tinted by the skin) and red / yellow
    // that read on light and dark skins (drawn by the plugin, see DrawChainText)
    static int TextColor(ChainStatus tone) {
        return tone == ChainStatus::Bad ? 0xD73A49 : tone == ChainStatus::Warn ? 0xC08A1E : 0x8B949E;
    }
    static bool Painted(ChainStatus tone) { return tone == ChainStatus::Bad || tone == ChainStatus::Warn; }

    // Lines a text needs at a width (explicit line breaks count as well)
    static int Lines(const tstring& text, int width) {
        int lines = 0;
        size_t start = 0;
        for (;;) {
            size_t nl = text.find(AR_T('\n'), start);
            tstring part = text.substr(start, nl == tstring::npos ? tstring::npos : nl - start);
            size_t len = 0;
            for (TChar c : part) len += (sizeof(TChar) == 1 && ((unsigned char)c & 0xC0) == 0x80) ? 0 : 1;  // UTF-8
            lines += (int)(len * kCharW / (width * 0.92)) + 1;  // a little room for wider letters
            if (nl == tstring::npos) break;
            start = nl + 1;
        }
        return lines;
    }

    // Height of the panel for a view at a width
    static int HeightOf(const ChainView& v, int width) {
        int y = kStatusH;
        for (size_t i = 0; i < v.stages.size(); i++) {
            y += StageHeight(v.stages[i], width);
            if (i + 1 < v.stages.size()) y += kGap;
        }
        return y;
    }
    static int StageHeight(const ChainStage& s, int width) {
        int h = 20;  // the name
        for (auto& r : s.rows) h += RowLines(r, width) * kLine;
        return (std::max)(h, kIcon);
    }
    static int RowLines(const ChainRow& r, int width) {
        return r.key.empty() ? Lines(r.value, width - kText) : Lines(r.value, width - kText - kKeyW);
    }

    ChainPanel(IAIMPServiceUI* ui, IAIMPUIForm* form, IAIMPUIWinControl* parent, int x, int y, int width)
        : ui_(ui), form_(form), parent_(parent), x_(x), y_(y), w_(width) {
        statusEvents_ = MakePaint(status_, [this](HCANVAS c, const RECT& r) { DrawChainStatus(c, r, view_.status); });
        PlaceControl(status_.Get(), x_ + w_ / 2 - 14, y_, 28, 28);
        summary_ = NewLabel(true);
        if (summary_) summary_->SetValueAsInt32(AIMPUI_LABEL_PROPID_TEXTALIGN, AIMPUI_ALIGN_CENTER);
        PlaceControl(summary_.Get(), x_, y_ + 32, w_, 18);
        railEvents_ = MakePaint(rail_, [this](HCANVAS c, const RECT& r) { DrawRail(c, r); });
    }
    ~ChainPanel() {
        std::vector<PaintEvents*> all = {statusEvents_, railEvents_};
        for (auto& c : colored_) all.push_back(c->events);
        for (PaintEvents* e : all)
            if (e) {
                e->draw = nullptr;
                e->Release();
            }
    }

    int Height() const { return height_; }
    const ChainView& View() const { return view_; }
    IAIMPUIControl* FirstStep() const { return titles_.empty() ? nullptr : titles_[0].Get(); }

    // Shows a view: moves the labels to their places, the rail paint box follows the height
    void Show(const ChainView& v) {
        view_ = v;
        SetUiText(summary_.Get(), AIMPUI_LABEL_PROPID_TEXT, v.summary);
        // In a scrolled parent (the Output tab) AIMP takes new positions relative to the visible area, not to the
        // top of the content: the status sign never moves, so its current position tells how far it is scrolled
        int dy = ScrollShift();
        int y = y_ + dy + kStatusH;
        size_t nTitle = 0, nKey = 0, nValue[4] = {0, 0, 0, 0}, nColored = 0;
        marks_.clear();
        int top = y;
        for (size_t i = 0; i < v.stages.size(); i++) {
            const ChainStage& s = v.stages[i];
            marks_.push_back({(float)(y - top + kIcon / 2), s.status, ChainIconGlyphs(s.icon, s.status)});
            auto& t = Pool(titles_, nTitle++, true);
            SetUiText(t.Get(), AIMPUI_LABEL_PROPID_TEXT, s.title);
            Put(t.Get(), x_ + kText, y + 2, w_ - kText, 18);
            int ry = y + 20;
            for (auto& r : s.rows) {
                int lines = RowLines(r, w_);
                int vx = x_ + kText, vw = w_ - kText;
                if (!r.key.empty()) {
                    auto& k = Pool(keys_, nKey++, false);
                    SetUiText(k.Get(), AIMPUI_LABEL_PROPID_TEXT, r.key);
                    Put(k.Get(), vx, ry, kKeyW - 6, kLine);
                    vx += kKeyW;
                    vw -= kKeyW;
                }
                if (Painted(r.tone)) {  // red / yellow: drawn by the plugin in the exact colour
                    ColoredText* c = Colored(nColored++);
                    if (c && c->box) {
                        c->text = r.value;
                        c->rgb = (uint32_t)TextColor(r.tone);
                        c->lines = lines;
                        PlaceControl(c->box.Get(), vx, ry, vw, lines * kLine);
                        c->box->SetValueAsInt32(AIMPUI_CONTROL_PROPID_VISIBLE, 1);
                        c->box->Invalidate();
                    }
                    ry += lines * kLine;
                    continue;
                }
                int tone = (int)r.tone > 3 ? 1 : (int)r.tone;
                auto& val = Pool(values_[tone], nValue[tone]++, false);
                if (!val) continue;
                SetUiText(val.Get(), AIMPUI_LABEL_PROPID_TEXT, r.value);
                val->SetValueAsInt32(AIMPUI_LABEL_PROPID_WORDWRAP, lines > 1 ? 1 : 0);
                Put(val.Get(), vx, ry, vw, lines * kLine);
                ry += lines * kLine;
            }
            y += StageHeight(s, w_);
            if (i + 1 < v.stages.size()) y += kGap;
        }
        height_ = y - y_ - dy;
        Hide(titles_, nTitle);
        Hide(keys_, nKey);
        for (int t = 0; t < 4; t++) Hide(values_[t], nValue[t]);
        for (size_t i = nColored; i < colored_.size(); i++)
            if (colored_[i]->box) colored_[i]->box->SetValueAsInt32(AIMPUI_CONTROL_PROPID_VISIBLE, 0);
        railH_ = y - top;
        PlaceControl(rail_.Get(), x_, top, kIcon + 4, (std::max)(1, railH_));
        if (rail_) rail_->Invalidate();
        if (status_) status_->Invalidate();
    }

private:
    // How far the parent is scrolled (0 in the chain window): where the status sign is now minus where it was put
    int ScrollShift() const {
        TAIMPUIControlPlacement p;
        memset(&p, 0, sizeof(p));
        if (!status_ || Failed(status_->GetPlacement(&p))) return 0;
        return p.Bounds.top - y_;
    }

    IAIMPServiceUI* ui_;
    IAIMPUIForm* form_;
    IAIMPUIWinControl* parent_;
    int x_, y_, w_, height_ = 0, railH_ = 0;
    Ptr<IAIMPUIPaintBox> status_, rail_;
    PaintEvents *statusEvents_ = nullptr, *railEvents_ = nullptr;
    Ptr<IAIMPUILabel> summary_;
    std::vector<Ptr<IAIMPUILabel>> titles_, keys_, values_[4];  // values by tone: Ok, Info (Warn, Bad: colored_)
    struct ColoredText {
        Ptr<IAIMPUIPaintBox> box;
        PaintEvents* events = nullptr;
        tstring text;
        uint32_t rgb = 0;
        int lines = 1;
    };
    std::vector<std::unique_ptr<ColoredText>> colored_;
    std::vector<ChainMark> marks_;
    ChainView view_;

    PaintEvents* MakePaint(Ptr<IAIMPUIPaintBox>& box, std::function<void(HCANVAS, const RECT&)> draw) {
        PaintEvents* e = new PaintEvents();
        e->AddRef();
        e->draw = std::move(draw);
        if (Failed(ui_->CreateControl(form_, parent_, nullptr, static_cast<AimpDrawEvents*>(e), IID_IAIMPUIPaintBox, box.OutV())))
            box.Reset();
        return e;
    }
    ColoredText* Colored(size_t i) {
        while (colored_.size() <= i) {
            std::unique_ptr<ColoredText> c(new ColoredText());
            ColoredText* p = c.get();
            c->events = MakePaint(c->box, [p](HCANVAS cv, const RECT& r) {
                DrawChainText(cv, r, p->text, p->rgb, (r.bottom - r.top) / (float)(std::max)(1, p->lines));
            });
            colored_.push_back(std::move(c));
        }
        return colored_[i].get();
    }
    Ptr<IAIMPUILabel> NewLabel(bool bold) {
        Ptr<IAIMPUILabel> l;
        if (Failed(ui_->CreateControl(form_, parent_, nullptr, nullptr, IID_IAIMPUILabel, l.OutV())) || !l) return Ptr<IAIMPUILabel>();
        l->SetValueAsInt32(AIMPUI_LABEL_PROPID_AUTOSIZE, 0);
        l->SetValueAsInt32(AIMPUI_LABEL_PROPID_WORDWRAP, 0);
        l->SetValueAsInt32(AIMPUI_LABEL_PROPID_TEXTALIGNVERT, AIMPUI_ALIGN_NEAR);
        if (bold) l->SetValueAsInt32(AIMPUI_LABEL_PROPID_TEXTSTYLE, AIMPUI_FLAGS_FONT_BOLD);
        return l;
    }
    Ptr<IAIMPUILabel>& Pool(std::vector<Ptr<IAIMPUILabel>>& pool, size_t i, bool bold) {
        while (pool.size() <= i) {
            pool.push_back(NewLabel(bold));
            // names of the details in grey, values in the tone of their pool (normal ones in the skin's colour)
            ChainStatus tone = &pool == &keys_ ? ChainStatus::Info : ChainStatus::Ok;
            for (int t = 1; t < 4; t++)
                if (&pool == &values_[t]) tone = (ChainStatus)t;
            if (pool.back() && tone != ChainStatus::Ok) pool.back()->SetValueAsInt32(AIMPUI_LABEL_PROPID_TEXTCOLOR, TextColor(tone));
        }
        return pool[i];
    }
    static void Put(IAIMPUILabel* l, int x, int y, int w, int h) {
        if (!l) return;
        PlaceControl(l, x, y, w, h);
        l->SetValueAsInt32(AIMPUI_CONTROL_PROPID_VISIBLE, 1);
    }
    static void Hide(std::vector<Ptr<IAIMPUILabel>>& pool, size_t from) {
        for (size_t i = from; i < pool.size(); i++)
            if (pool[i]) pool[i]->SetValueAsInt32(AIMPUI_CONTROL_PROPID_VISIBLE, 0);
    }

    void DrawRail(HCANVAS canvas, const RECT& r) {
        // design units -> pixels (AIMP scales the controls with the DPI)
        float scale = railH_ > 0 ? (r.bottom - r.top) / (float)railH_ : 1.0f;
        std::vector<ChainMark> m = marks_;
        for (auto& x : m) x.cy = r.top + x.cy * scale;
        DrawChainMarks(canvas, m, r.left + (r.right - r.left) / 2.0f, kIcon / 2.0f * scale, true);
    }
};

// Form events of the chain window: it is not modal, so AIMP stays usable while it is open
class ChainFormEvents : public ComObject<IAIMPUIFormEvents> {
    std::function<void()> closing_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPUIFormEvents); }

public:
    explicit ChainFormEvents(std::function<void()> f) : closing_(std::move(f)) {}
    void WINAPI OnActivated(IAIMPUIForm*) override {}
    void WINAPI OnDeactivated(IAIMPUIForm*) override {}
    void WINAPI OnCreated(IAIMPUIForm*) override {}
    void WINAPI OnDestroyed(IAIMPUIForm*) override {}
    void WINAPI OnCloseQuery(IAIMPUIForm*, BOOL* canClose) override {
        if (canClose) *canClose = 1;
        if (closing_) closing_();
    }
    void WINAPI OnLocalize(IAIMPUIForm*) override {}
    void WINAPI OnShortCut(IAIMPUIForm*, WORD, WORD, BOOL*) override {}
};

// ---------------------------------------------------------------- the window
class ChainWindow {
public:
    static const int kPanelW = 470, kPad = 12, kMaxH = 600;  // kMaxH: only when the screen is not known
    static int Width() { return kPanelW + 2 * kPad + 20; }  // room for the vertical scroll bar

private:
    Engine* engine_;
    Ptr<IAIMPServiceUI> ui_;
    Ptr<IAIMPUIForm> form_;
    Ptr<IAIMPUIChangeEvents> events_;
    Ptr<IAIMPUIFormEvents> formEvents_;
    Ptr<IAIMPUIScrollBox> box_;
    std::unique_ptr<ChainPanel> panel_;
    Ptr<IAIMPUIButton> btClose_;
    HWND owner_ = (HWND)0;
    int boxH_ = 0;
    bool havePos_ = false;  // reopened after an AIMP restart: at the place it had before
    int posX_ = 0, posY_ = 0;
    int ticks_ = 0;

    Ptr<IAIMPUIButton> Button(const tstring& caption, int x, int y, int w) {
        Ptr<IAIMPUIButton> b;
        if (Failed(ui_->CreateControl(form_.Get(), form_.Get(), nullptr, events_.Get(), IID_IAIMPUIButton, b.OutV())) || !b) return b;
        PlaceControl(b.Get(), x, y, w, 25);
        SetUiText(b.Get(), AIMPUI_BUTTON_PROPID_CAPTION, caption);
        return b;
    }

    void OnChanged(IUnknown* sender) {
        if (Same(sender, btClose_.Get()) && form_) form_->Close();
    }
    static bool Same(IUnknown* a, IUnknown* b) {
        if (!a || !b) return false;
        if (a == b) return true;
        Ptr<IUnknown> ua, ub;
        a->QueryInterface(IID_IUnknown, ua.OutV());
        b->QueryInterface(IID_IUnknown, ub.OutV());
        return ua && ua.Get() == ub.Get();
    }

    void Release() {
        panel_.reset();
        btClose_.Reset();
        box_.Reset();
        if (form_) form_->Destroy(0);
        form_.Reset();
        events_.Reset();
        formEvents_.Reset();
        ui_.Reset();
    }

    // A closed window: AIMP only hides it; it is destroyed when the next one opens or the plugin ends (never
    // from its own close event, and no task queued for later - AIMP 4 does not close with one pending)
    static ChainWindow*& Closed() {
        static ChainWindow* w = nullptr;
        return w;
    }
    static void DropClosed() {
        if (!Closed()) return;
        Closed()->Release();
        delete Closed();
        Closed() = nullptr;
    }

public:
    explicit ChainWindow(Engine* e) : engine_(e) {}

    // The window that is open right now (there is only one)
    static ChainWindow*& Current() {
        static ChainWindow* w = nullptr;
        return w;
    }

    // AIMP's main window (owner of the window when it is opened from the menu)
    static HWND MainWindow() {
        HWND h = (HWND)0;
        auto disp = Service<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher);
        if (disp) disp->Send(AIMP_MSG_PROPERTY_HWND, AIMP_MPH_MAINFORM, &h);
        return h;
    }

    // Opens the window (not modal: AIMP can be used while it is open) and returns at once. Only one at a time;
    // a second call brings the open one to the front.
    static void Open(Engine* engine, HWND owner, const tstring& place = tstring()) {
        if (Current()) {
            Current()->ShowForm();
            return;
        }
        DropClosed();
        ChainWindow* w = new ChainWindow(engine);
        int x = 0, y = 0;
        if (ParsePlace(place, x, y)) {
            w->havePos_ = true;
            w->posX_ = x;
            w->posY_ = y;
        }
        if (!w->Show(owner)) delete w;
    }

    // Where the open window is ("x,y" of its top left corner on the screen), to open it there again after an
    // AIMP restart; "1" if not known; empty if no window is open
    static tstring Place() {
        ChainWindow* w = Current();
        if (!w || !w->form_) return tstring();
#ifdef _WIN32
        RECT r;
        HWND h = w->form_->GetHandle();
        if (h && GetWindowRect(h, &r)) return Num(r.left) + AR_T(",") + Num(r.top);
#endif
        return AR_T("1");
    }

    static bool ParsePlace(const tstring& s, int& x, int& y) {
        size_t c = s.find(AR_T(','));
        if (c == tstring::npos || c == 0 || c + 1 >= s.size()) return false;
        tstring a = s.substr(0, c), b = s.substr(c + 1);
        auto num = [](const tstring& t, int& v) {
            size_t i = 0;
            bool neg = !t.empty() && t[0] == AR_T('-');
            if (neg) i = 1;
            if (i >= t.size()) return false;
            long long n = 0;
            for (; i < t.size(); i++) {
                if (t[i] < AR_T('0') || t[i] > AR_T('9') || n > 1000000) return false;
                n = n * 10 + (t[i] - AR_T('0'));
            }
            v = (int)(neg ? -n : n);
            return true;
        };
        return num(a, x) && num(b, y);
    }

    // Ends the open or closed window (the plugin is finalized)
    static void CloseAll() {
        if (ChainWindow* w = Current()) {
            Current() = nullptr;
            w->Release();
            delete w;
        }
        DropClosed();
    }

private:
    // Shows the form without a modal loop and brings it to the front (IAIMPUIForm2 is AIMP 6 only)
    void ShowForm() {
        if (!form_) return;
        Ptr<IAIMPUIForm2> f2;
        if (Succeeded(form_->QueryInterface(IID_IAIMPUIForm2, f2.OutV())) && f2) {
            f2->Show();
            f2->BringToTop();
            return;
        }
        form_->SetValueAsInt32(AIMPUI_CONTROL_PROPID_VISIBLE, 1);
#ifdef _WIN32
        HWND h = form_->GetHandle();
        if (h) SetForegroundWindow(h);
#endif
    }

    // The user closed it (Close, Escape or X)
    void OnClosing() {
        if (Current() != this) return;
        Current() = nullptr;
        Closed() = this;
        Log(AR_T("Audio chain window closed"));
    }

    bool Show(HWND owner) {
        ui_ = Service<IAIMPServiceUI>(IID_IAIMPServiceUI);
        if (!ui_) return false;
        ChangeHandler* h = new ChangeHandler([this](IUnknown* s) { OnChanged(s); });
        h->AddRef();
        *events_.Out() = h;
        ChainFormEvents* fe = new ChainFormEvents([this] { OnClosing(); });
        fe->AddRef();
        *formEvents_.Out() = fe;
        auto name = MakeString(AR_T("PreventResamplingChain"));
        if (Failed(ui_->CreateForm(owner, AIMPUI_SERVICE_CREATEFORM_FLAGS_REGULAR, name.Get(), formEvents_.Get(), form_.Out())) || !form_) {
            Release();
            return false;
        }
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_BORDERSTYLE, AIMPUI_FLAGS_BORDERSTYLE_DIALOG);
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_BORDERICONS, AIMPUI_FLAGS_BORDERICON_SYSTEMMENU);
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_CLOSEBYESCAPE, 1);
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_SHOWONTASKBAR, 0);
        SetUiText(form_.Get(), AIMPUI_FORM_PROPID_CAPTION, Tr("Audio chain") + AR_T(" – " AR_PLUGIN_NAME));
        // As high as the chain when it opens (no scrolling; only a chain higher than the screen scrolls), centred
        // on the owner
        ChainView v = BuildChain(GatherChain(*engine_));
        owner_ = owner;
        int boxH = BoxHeight(v);
        boxH_ = boxH;
        int w = Width(), hgt = boxH + 44;
        TAIMPUIControlPlacement pl;
        memset(&pl, 0, sizeof(pl));
        pl.Alignment = ualNone;
        int x = 200, y = 120;
#ifdef _WIN32
        RECT o;
        if (owner && GetWindowRect(owner, &o)) {
            x = (o.left + o.right) / 2 - w / 2;
            y = (std::max)(0L, (o.top + o.bottom) / 2 - hgt / 2);
        }
#endif
#ifdef _WIN32
        if (havePos_) {  // the old place, as long as it is still on a screen
            RECT want = {posX_, posY_, posX_ + w + 16, posY_ + hgt + 39};
            HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONULL);
            MONITORINFO mi;
            memset(&mi, 0, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                x = (std::max)((int)mi.rcWork.left, (std::min)(posX_, (int)mi.rcWork.right - (w + 16)));
                y = (std::max)((int)mi.rcWork.top, (std::min)(posY_, (int)mi.rcWork.bottom - (hgt + 39)));
            }
        }
#endif
        pl.Bounds.left = x;
        pl.Bounds.top = y;
        pl.Bounds.right = x + w + 16;     // borders and caption of a dialog window
        pl.Bounds.bottom = y + hgt + 39;
        form_->SetPlacement(pl);
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_CLIENTWIDTH, w);
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_CLIENTHEIGHT, hgt);
        IAIMPUIWinControl* parent = form_.Get();
        if (Succeeded(ui_->CreateControl(form_.Get(), form_.Get(), nullptr, nullptr, IID_IAIMPUIScrollBox, box_.OutV())) && box_) {
            box_->SetValueAsInt32(AIMPUI_SCROLLBOX_PROPID_BORDERS, 0);
            PlaceControl(box_.Get(), 0, 0, w, boxH);
            parent = box_.Get();
        }
        panel_.reset(new ChainPanel(ui_.Get(), form_.Get(), parent, kPad, kPad, kPanelW));
        btClose_ = Button(Tr("Close"), w - 122, boxH + 8, 110);  // it refreshes itself, no Refresh button
        panel_->Show(v);
        Current() = this;
        Log(AR_T("Audio chain window opened"));
        ShowForm();
        return true;
    }

public:

    // Highest box for the screen of the owner: its work area (design units) minus caption, buttons and borders
    static int MaxBoxHeight(HWND owner) {
        int h = kMaxH;
#ifdef _WIN32
        HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) {
            int dpi = 96;
            typedef UINT(WINAPI * DpiForWindow)(HWND);
            auto fn = (DpiForWindow)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
            if (fn && owner) dpi = (int)fn(owner);
            if (dpi < 96) {
                HDC dc = GetDC(nullptr);
                dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
                if (dc) ReleaseDC(nullptr, dc);
            }
            int screen = (mi.rcWork.bottom - mi.rcWork.top) * 96 / (std::max)(96, dpi);
            h = (std::max)(300, screen - 44 - 60);
        }
#else
        h = 900;
#endif
        return h;
    }

    // Box as high as the chain, at most as high as the screen allows
    int BoxHeight(const ChainView& v) const {
        return (std::min)(ChainPanel::HeightOf(v, kPanelW) + 2 * kPad + 8, MaxBoxHeight(owner_));
    }

    // The chain got higher or lower (longer texts, a step more or less): the window follows, so it never needs
    // a scroll bar while the chain fits on the screen. The top edge stays; a window that would reach below the
    // screen moves up.
    void FitHeight(const ChainView& v) {
        int boxH = BoxHeight(v);
        if (boxH == boxH_ || !form_) return;
        int oldH = boxH_ + 44;
        boxH_ = boxH;
        int w = Width();
#ifdef _WIN32
        HWND h = form_->GetHandle();
        RECT c0;
        int px = 0;  // the new client height in pixels (the window may be scaled for the screen's DPI)
        if (h && GetClientRect(h, &c0) && oldH > 0) px = (int)((c0.bottom - c0.top) * (long long)(boxH + 44) / oldH);
#endif
        if (box_) PlaceControl(box_.Get(), 0, 0, w, boxH);
        if (btClose_) PlaceControl(btClose_.Get(), w - 122, boxH + 8, 110, 25);
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_CLIENTHEIGHT, boxH + 44);
#ifdef _WIN32
        RECT r, c;
        // AIMP 4 and 5 keep the size of a form that is on screen: resized by Windows then
        if (h && px > 0 && GetClientRect(h, &c) && GetWindowRect(h, &r) && abs((c.bottom - c.top) - px) > 1) {
            int dh = px - (c.bottom - c.top);
            SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top + dh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (h && GetWindowRect(h, &r)) {
            MONITORINFO mi;
            memset(&mi, 0, sizeof(mi));
            mi.cbSize = sizeof(mi);
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            if (mon && GetMonitorInfoW(mon, &mi) && r.bottom > mi.rcWork.bottom) {
                int top = (std::max)(mi.rcWork.top, r.top - (r.bottom - mi.rcWork.bottom));
                SetWindowPos(h, nullptr, r.left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
#endif
        Log(AR_T("Audio chain window: new height ") + Num(boxH));
    }

    // Collects the facts again and shows them
    void Refresh() {
        if (!form_ || !panel_) return;
        ChainView v = BuildChain(GatherChain(*engine_));
        FitHeight(v);
        panel_->Show(v);
    }

    // Once a second while AIMP plays: refreshed every 2 s (devices, AIMP's effects may change meanwhile); also
    // when a track starts, pauses or stops (Plugin.cpp)
    void Tick() {
        if (++ticks_ % 2 == 0) Refresh();
    }

    const ChainView& View() const { static ChainView none; return panel_ ? panel_->View() : none; }

};

}  // namespace ar
