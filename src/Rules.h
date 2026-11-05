// Rules that decide per track what the plugin does (General tab): the highest rate and the exceptions.
// Plain functions without AIMP or the operating system, so the mock host can test them directly.
#pragma once
#include "Common.h"

namespace ar {

// Highest rate of the track's family (multiples of 44.1 or 48 kHz) that is not above the limit:
// 192 -> 96, 176.4 -> 88.2, 352.8 -> 88.2 kHz with a limit of 96 kHz. A rate of no family (e.g. 64 kHz)
// above the limit becomes the limit itself. limit 0 = no limit.
inline uint32_t LimitRate(uint32_t hz, uint32_t limit) {
    if (!limit || hz <= limit) return hz;
    uint32_t base = (hz % 44100 == 0) ? 44100 : (hz % 48000 == 0) ? 48000 : 0;
    if (!base || base > limit) return limit;
    uint32_t r = base;
    while (r * 2 <= limit && r * 2 < hz) r *= 2;
    return r;
}

inline tstring FileExtension(const tstring& file) {
    size_t dot = file.find_last_of(AR_T('.')), sep = file.find_last_of(AR_T("\\/"));
    if (dot == tstring::npos || (sep != tstring::npos && dot < sep)) return tstring();
    return Lower(file.substr(dot + 1));
}

// Lossy formats (MP3, AAC, Ogg Vorbis, Opus, WMA, Musepack ...): AIMP's codec name first - it knows what is
// inside a container (.m4a can be ALAC, .ogg can be FLAC) - then the file extension.
inline bool IsLossy(const tstring& codec, const tstring& file) {
    static const TChar* lossless[] = {AR_T("lossless"), AR_T("flac"), AR_T("alac"), AR_T("ape"), AR_T("monkey"),
                                      AR_T("wavpack"), AR_T("pcm"), AR_T("wav"), AR_T("aif"), AR_T("tta"),
                                      AR_T("tak"), AR_T("dsd"), AR_T("dsf"), AR_T("dff"), AR_T("optimfrog"),
                                      AR_T("shorten"), AR_T("truehd"), AR_T("mlp"), AR_T("master audio")};
    static const TChar* lossy[] = {AR_T("mp3"), AR_T("mpeg"), AR_T("layer"), AR_T("mp2"), AR_T("aac"),
                                   AR_T("vorbis"), AR_T("opus"), AR_T("wma"), AR_T("musepack"), AR_T("mpc"),
                                   AR_T("ac3"), AR_T("ac-3"), AR_T("dts"), AR_T("speex"), AR_T("atrac"),
                                   AR_T("amr")};
    for (auto* k : lossless)
        if (ContainsI(codec, k)) return false;
    for (auto* k : lossy)
        if (ContainsI(codec, k)) return true;
    static const TChar* lossyExt[] = {AR_T("mp1"), AR_T("mp2"), AR_T("mp3"), AR_T("mpga"), AR_T("aac"), AR_T("m4a"),
                                      AR_T("m4b"), AR_T("mp4"), AR_T("ogg"), AR_T("oga"), AR_T("opus"), AR_T("wma"),
                                      AR_T("mpc"), AR_T("mp+"), AR_T("mpp"), AR_T("ac3"), AR_T("eac3"), AR_T("spx"),
                                      AR_T("amr"), AR_T("ra"), AR_T("rm"), AR_T("weba"), AR_T("webm")};
    if (!codec.empty()) return false;  // a codec AIMP names that is in neither list: not treated as lossy
    tstring ext = FileExtension(file);
    for (auto* e : lossyExt)
        if (ext == e) return true;
    return false;
}

// Exception list "D:\Audiobooks; Podcasts; Chill-out" (General tab). An entry is
//  - a full path (has a drive or starts with a separator): the file is in that folder or below it,
//  - otherwise a folder name anywhere in the file's path, or the name of the playlist the track plays from.
// Returns the entry that matched (empty = none). Full paths: Windows without case, Linux with case (like the
// file systems); folder and playlist names always without case.
inline tstring MatchSkipList(const tstring& list, const tstring& file, const tstring& playlist) {
    auto norm = [](tstring s) {
        for (auto& c : s)
            if (c == AR_T('\\')) c = AR_T('/');
        while (s.size() > 1 && s.back() == AR_T('/')) s.pop_back();
#ifdef _WIN32
        s = Lower(s);
#endif
        return s;
    };
    tstring path = norm(file);
    for (auto& entry : Split(list, AR_T(';'))) {
        tstring e = norm(entry);
        if (e.empty()) continue;
        bool full = e[0] == AR_T('/') || (e.size() >= 2 && e[1] == AR_T(':'));
        if (full) {
            if (e.back() != AR_T('/')) e += AR_T('/');
            if (path.compare(0, e.size(), e) == 0) return entry;
            continue;
        }
        if (!playlist.empty() && EqualsI(Trim(playlist), Trim(entry))) return entry;
        if (ContainsI(path, AR_T("/") + e + AR_T("/"))) return entry;  // "Podcasts" or "Music/Podcasts"
    }
    return tstring();
}

}  // namespace ar
