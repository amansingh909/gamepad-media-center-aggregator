// Standalone logic test: the order of a title's sources (quality, the PS4 heavy rule, torrents
// before links, seeders and size), and the 10-bit and size parsing it reads.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_source_order.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <api/stremio/source_order.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

using stremio::SourceKey;
static const int64_t GB = int64_t(1) << 30;

static SourceKey tor(int quality, int seeders, int64_t size, bool heavy = false) {
    SourceKey k;
    k.playable = true;
    k.quality = quality;
    k.torrent = true;
    k.seeders = seeders;
    k.sizeBytes = size;
    k.heavy = heavy;
    return k;
}
static SourceKey lnk(int quality, bool cached = true, bool heavy = false) {
    SourceKey k;
    k.playable = true;
    k.quality = quality;
    k.cached = cached;
    k.heavy = heavy;
    return k;
}
static SourceKey grey(int quality) {
    SourceKey k;
    k.quality = quality;
    return k;
}

// the labels of `rows` after the sort the backend does
static std::string sorted(std::vector<std::pair<std::string, SourceKey>> rows, bool ps4) {
    std::stable_sort(rows.begin(), rows.end(), [ps4](const auto& x, const auto& y) {
        return stremio::compareSources(x.second, y.second, ps4) < 0;
    });
    std::string out;
    for (const auto& r : rows) out += (out.empty() ? "" : " ") + r.first;
    return out;
}

int main() {
    using namespace stremio;

    // 10-bit: the tags addons print; an HDRip is a rip, not HDR
    CHECK(parseTenBit("Show.S01E01.1080p.x265.10bit-GRP"));
    CHECK(parseTenBit("2160p HDR10+ HEVC"));
    CHECK(parseTenBit("1080p HDR DV"));
    CHECK(parseTenBit("Hi10P"));
    CHECK(parseTenBit("10-bit"));
    CHECK(parseTenBit("10 bit"));
    CHECK(!parseTenBit("1080p HDRip x264"));
    CHECK(!parseTenBit("720p x264 AAC"));
    CHECK(!parseTenBit(""));

    // codec and 10-bit read name + title + the torrent file's name (streamToMedia joins them)
    {
        const std::string text = std::string("Addon") + " " + "Show 1080p" + " " + "Show.S01E01.1080p.x265.10bit.mkv";
        CHECK(parseCodecLabel(text) == "HEVC");
        CHECK(parseTenBit(text));
        CHECK(!heavyOnPs4(parseCodecLabel(text), parseTenBit(text), "1080p", true));
        CHECK(heavyOnPs4(parseCodecLabel(text), parseTenBit(text), "4K", true));
        CHECK(compareSources([&] { SourceKey k = tor(6, 50, GB); k.heavy = heavyOnPs4(parseCodecLabel(text), parseTenBit(text), "4K", true); return k; }(),
                             tor(4, 5, GB), true) > 0);
        const std::string light = "Addon Show 1080p Show.S01E01.1080p.x264.mkv";
        CHECK(parseCodecLabel(light) == "H.264");
        CHECK(!heavyOnPs4(parseCodecLabel(light), parseTenBit(light), "1080p", false));
        CHECK(parseCodecLabel("Addon Show 1080p ").empty());
        CHECK(parseCodecLabel("x264 AV1") == "AV1");
    }

    // sizes as parseSizeLabel() writes them, binary multiples
    CHECK(parseSizeBytes("8.4 GB") == 9019431321LL);
    CHECK(parseSizeBytes("700 MB") == 734003200LL);
    CHECK(parseSizeBytes("1.2 TB") == 1319413953331LL);
    CHECK(parseSizeBytes("129.2 MB") == 135476019LL);
    CHECK(parseSizeBytes("") == -1);
    CHECK(parseSizeBytes("GB") == -1);
    CHECK(parseSizeBytes("12 KB") == -1);

    // heavy on a PS4: AV1, and HEVC or 10-bit at 4K or 1440p (diag J: 1080p HDR HEVC plays)
    CHECK(!heavyOnPs4("HEVC", false, "1080p", true));
    CHECK(!heavyOnPs4("HEVC", false, "720p", true));
    CHECK(heavyOnPs4("AV1", false, "720p", true));
    CHECK(heavyOnPs4("AV1", false, "1080p", true));
    CHECK(!heavyOnPs4("H.264", true, "1080p", true));  // Hi10P at 1080p
    CHECK(heavyOnPs4("H.264", true, "4K", true));
    CHECK(!heavyOnPs4("H.264", false, "1080p", true));
    CHECK(!heavyOnPs4("H.264", false, "4K", true));
    CHECK(!heavyOnPs4("", false, "1080p", true));      // no codec named: most often H.264
    CHECK(!heavyOnPs4("HEVC", false, "SD", true));
    CHECK(!heavyOnPs4("HEVC", true, "1080p", true));
    CHECK(heavyOnPs4("HEVC", false, "4K", true));
    CHECK(heavyOnPs4("HEVC", true, "4K", true));
    CHECK(heavyOnPs4("HEVC", false, "1440p", true));
    CHECK(heavyOnPs4("HEVC", false, "1080p", false));  // the 8-bit HEVC switch off: all HEVC heavy

    // 1. playable first, then quality
    CHECK(compareSources(lnk(2), grey(6), false) < 0);
    CHECK(compareSources(tor(3, 500, GB), tor(4, 1, GB), false) > 0);
    // 3. same quality: a torrent before a link, even a poorly seeded one
    CHECK(compareSources(tor(4, 3, -1), lnk(4), false) < 0);
    CHECK(compareSources(lnk(4), tor(4, -1, -1), false) > 0);
    // 4. 10 or more seeders first, smallest file first, ties to more seeders
    CHECK(compareSources(tor(4, 10, 3 * GB), tor(4, 9, GB), false) < 0);
    CHECK(compareSources(tor(4, 50, 2 * GB), tor(4, 12, GB), false) > 0);
    CHECK(compareSources(tor(4, 50, GB), tor(4, 12, GB), false) < 0);
    CHECK(compareSources(tor(4, 50, -1), tor(4, 12, GB), false) > 0);  // unknown size last
    // 5. then fewer than 10 or unknown seeders, most seeders first, ties to the smaller file
    CHECK(compareSources(tor(4, 9, 3 * GB), tor(4, 2, GB), false) < 0);
    CHECK(compareSources(tor(4, -1, GB), tor(4, 0, 3 * GB), false) > 0);
    CHECK(compareSources(tor(4, 5, 2 * GB), tor(4, 5, GB), false) > 0);
    // 6. links: cached debrid first, else the addons' order
    CHECK(compareSources(lnk(4, true), lnk(4, false), false) < 0);
    CHECK(compareSources(lnk(4), lnk(4), false) == 0);
    CHECK(compareSources(tor(4, 7, GB), tor(4, 7, GB), false) == 0);
    // 2. on a PS4, light before heavy at any quality; elsewhere quality decides
    CHECK(compareSources(tor(3, 20, GB), tor(6, 200, 5 * GB, true), true) < 0);
    CHECK(compareSources(tor(3, 20, GB), tor(6, 200, 5 * GB, true), false) > 0);
    CHECK(compareSources(lnk(4, true, true), lnk(3), true) > 0);
    // grey rows: quality only, never the heavy rule
    CHECK(compareSources(grey(4), grey(6), true) > 0);
    CHECK(compareSources(grey(4), grey(4), true) == 0);

    // a title's whole list (4K=6, 1080p=4, 720p=3)
    std::vector<std::pair<std::string, SourceKey>> rows = {
        {"link1080", lnk(4)},
        {"t1080small", tor(4, 15, GB)},
        {"t1080big", tor(4, 300, 4 * GB)},
        {"t1080poor", tor(4, 4, GB / 2)},
        {"t4kHevc", tor(6, 900, 20 * GB, true)},
        {"t720", tor(3, 60, GB / 2)},
        {"t1080hevc", tor(4, 80, GB, true)},
        {"grey", grey(6)},
    };
    CHECK(sorted(rows, false) == "t4kHevc t1080hevc t1080small t1080big t1080poor link1080 t720 grey");
    CHECK(sorted(rows, true) == "t1080small t1080big t1080poor link1080 t720 t4kHevc t1080hevc grey");

    if (failures == 0) printf("test_source_order: all passed\n");
    return failures ? 1 : 0;
}
