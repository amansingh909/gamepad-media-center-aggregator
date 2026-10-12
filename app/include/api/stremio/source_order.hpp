// The order of a title's sources: the source list shows them in it, and every automatic pick
// follows it (auto-play, binge and next episode take the first accessible row; a download falls
// back to the rows after the one chosen). Header-only and free of borealis, nlohmann and curl
// (tests/test_source_order.cpp).
#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace stremio {

/// Torrents with at least this many seeders count as well seeded: among those the smallest file
/// goes first (less space, a lower bitrate, a faster start on the console).
constexpr int wellSeeded = 10;

/// Set from the HEVC test on the console (GMCA diag H): true once 8-bit HEVC at 720p or less
/// plays smoothly on a PS4, which then ranks it with H.264.
constexpr bool ps4LightHevcPlays = false;

/// What the order reads from one source row.
struct SourceKey {
    bool playable = false;
    int quality = 0;         // qualityRank() of the row's resolution: higher is better
    bool heavy = false;      // heavyOnPs4(): after the light sources on a PS4
    bool torrent = false;
    int seeders = -1;        // -1: the addon printed none
    int64_t sizeBytes = -1;  // -1: the addon printed none
    bool cached = true;      // a link's debrid cache hint
};

/// True when a stream's text names 10-bit video: "10bit", "10-bit", "10 bit", "Hi10P", or HDR
/// ("HDR", "HDR10", "HDR10+"), which is 10-bit. "HDRip" is a rip, not HDR.
inline bool parseTenBit(const std::string& text) {
    std::string t = text;
    for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* tag : {"10bit", "10-bit", "10 bit", "hi10"})
        if (t.find(tag) != std::string::npos) return true;
    for (size_t p = t.find("hdr"); p != std::string::npos; p = t.find("hdr", p + 3)) {
        const size_t next = p + 3;
        if (next >= t.size() || !std::isalpha(static_cast<unsigned char>(t[next]))) return true;
    }
    return false;
}

/// A size label as parseSizeLabel() writes it ("8.4 GB", "700 MB", "1.2 TB") in bytes (binary
/// multiples), or -1 when it isn't one.
inline int64_t parseSizeBytes(const std::string& label) {
    size_t i = 0;
    int64_t whole = 0, frac = 0, scale = 1;
    bool digits = false;
    while (i < label.size() && std::isdigit(static_cast<unsigned char>(label[i]))) {
        whole = whole * 10 + (label[i++] - '0');
        digits = true;
        if (whole > 1000000) return -1;
    }
    if (i < label.size() && label[i] == '.') {
        ++i;
        while (i < label.size() && std::isdigit(static_cast<unsigned char>(label[i]))) {
            if (scale < 1000) {
                frac = frac * 10 + (label[i] - '0');
                scale *= 10;
            }
            ++i;
            digits = true;
        }
    }
    if (!digits) return -1;
    while (i < label.size() && label[i] == ' ') ++i;
    const std::string unit = label.substr(i);
    const int64_t mult = unit == "TB"   ? int64_t(1) << 40
                         : unit == "GB" ? int64_t(1) << 30
                         : unit == "MB" ? int64_t(1) << 20
                                        : 0;
    if (mult == 0) return -1;
    return whole * mult + frac * mult / scale;
}

/// HEVC, AV1 and 10-bit video decode in software on a PS4, at the CPU's limit, and are often
/// not drawn (a blue screen with sound): they go after the light sources there. With
/// `lightHevcPlays` (ps4LightHevcPlays), 8-bit HEVC at 720p or less counts as light.
inline bool heavyOnPs4(const std::string& codec, bool tenBit, const std::string& resolution, bool lightHevcPlays) {
    if (tenBit || codec == "AV1") return true;
    if (codec != "HEVC") return false;
    const bool light = resolution == "720p" || resolution == "480p" || resolution == "SD";
    return !(lightHevcPlays && light);
}

/// The order of two sources: negative when x goes first, positive when y does, 0 when neither
/// (the sort keeps the addons' order). Playable first; on a PS4 (`ps4`) light before heavy at
/// any quality; then the best quality; at the same quality a torrent before a link (no debrid:
/// a link from another addon often fails); torrents with wellSeeded or more seeders first,
/// smallest file first (ties: more seeders); then the others, most seeders first (ties: the
/// smaller file); links last, cached debrid first. An unknown size goes after the known ones.
inline int compareSources(const SourceKey& x, const SourceKey& y, bool ps4) {
    if (x.playable != y.playable) return x.playable ? -1 : 1;
    if (ps4 && x.playable && x.heavy != y.heavy) return x.heavy ? 1 : -1;
    if (x.quality != y.quality) return x.quality > y.quality ? -1 : 1;
    if (!x.playable) return 0;
    if (x.torrent != y.torrent) return x.torrent ? -1 : 1;
    if (!x.torrent) return x.cached == y.cached ? 0 : (x.cached ? -1 : 1);
    auto smaller = [](int64_t a, int64_t b) {
        if (a == b) return 0;
        if (a < 0) return 1;
        if (b < 0) return -1;
        return a < b ? -1 : 1;
    };
    const bool xWell = x.seeders >= wellSeeded, yWell = y.seeders >= wellSeeded;
    if (xWell != yWell) return xWell ? -1 : 1;
    if (xWell) {
        if (int s = smaller(x.sizeBytes, y.sizeBytes)) return s;
        return x.seeders == y.seeders ? 0 : (x.seeders > y.seeders ? -1 : 1);
    }
    if (x.seeders != y.seeders) return x.seeders > y.seeders ? -1 : 1;
    return smaller(x.sizeBytes, y.sizeBytes);
}

}  // namespace stremio
