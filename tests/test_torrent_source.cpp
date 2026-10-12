// Standalone logic test: Stremio torrent streams as source rows (the local server's URL for a
// stream and back, trackers, the download's extension, seeders, row order).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_source.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <vector>
#include <torrent/stremio_source.hpp>
#include <torrent/torrent_engine.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    using namespace torrent;
    const std::string H = "08ada5a7a6183aae1e09d831df6748d566095a10";
    const std::string base = "http://127.0.0.1:11470/";

    // trackers: only "tracker:" sources, no duplicates, at most 16
    std::vector<std::string> sources = {"tracker:udp://a.example:1337/announce", "dht:" + H,
                                        "tracker:udp://a.example:1337/announce", "tracker:", "tracker:https://b.example/announce"};
    std::vector<std::string> tr = trackersFromSources(sources);
    CHECK(tr.size() == 2);
    CHECK(tr.size() == 2 && tr[0] == "udp://a.example:1337/announce" && tr[1] == "https://b.example/announce");
    std::vector<std::string> many;
    for (int i = 0; i < 40; ++i) many.push_back("tracker:udp://t" + std::to_string(i) + ".example/announce");
    CHECK(trackersFromSources(many).size() == maxTrackers);

    // an info hash stream to the local URL and back
    std::string url = localStreamUrl(H, 5, {"tracker:udp://tracker.opentrackr.org:1337/announce"}, "Sintel.mp4");
    CHECK_EQ(url, base + H + "/5?tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce&f=Sintel.mp4");
    StreamRequest r;
    CHECK(parseLocalStreamUrl(url, r));
    CHECK_EQ(r.infoHash, H);
    CHECK(r.fileIndex == 5);
    CHECK(r.trackers.size() == 1 && r.trackers[0] == "udp://tracker.opentrackr.org:1337/announce");
    CHECK(r.fileHints.size() == 1 && r.fileHints[0] == "Sintel.mp4");

    // Torrentio's shape: uppercase hash, no sources, no file index, a name with brackets
    url = localStreamUrl("A779FA758F2229586EEBC2E5960EB32FE5659596", -1, {}, "Sintel.2010.720p.BluRay.x264.AAC-[YTS.MX].mp4");
    CHECK_EQ(url, base + "a779fa758f2229586eebc2e5960eb32fe5659596/-1?f=Sintel.2010.720p.BluRay.x264.AAC-%5BYTS.MX%5D.mp4");
    CHECK(parseLocalStreamUrl(url, r) && r.infoHash == "a779fa758f2229586eebc2e5960eb32fe5659596" && r.fileIndex == -1);
    CHECK_EQ(localStreamUrl(H, 0, {}, ""), base + H + "/0");
    CHECK_EQ(localStreamUrl(H, -7, {}, ""), base + H + "/-1");

    // not an info hash: no URL, the row stays grey
    CHECK_EQ(localStreamUrl(H.substr(1), 0, {}, ""), "");
    CHECK_EQ(localStreamUrl(H.substr(1) + "z", 0, {}, ""), "");
    CHECK_EQ(localStreamUrl("", 0, {}, ""), "");

    // a URL the server would refuse is no URL: the row stays grey
    CHECK_EQ(localStreamUrl(H, 999999, {}, ""), base + H + "/999999");
    CHECK_EQ(localStreamUrl(H, 1000000, {}, ""), "");
    CHECK_EQ(localStreamUrl(H, 2147483647, {}, ""), "");
    // an absurd file name only loses its hint; the URL still parses
    url = localStreamUrl(H, 0, {}, std::string(100000, 'a') + ".mkv");
    CHECK_EQ(url, base + H + "/0");
    CHECK(parseLocalStreamUrl(url, r));
    // an over-long tracker is dropped, the others stay
    std::vector<std::string> trs = {"tracker:udp://a.example:1337/announce", "tracker:udp://" + std::string(5000, 'x') + ".example/announce"};
    url = localStreamUrl(H, 0, trs, "");
    CHECK(parseLocalStreamUrl(url, r) && r.trackers.size() == 1 && r.trackers[0] == "udp://a.example:1337/announce");
    // however much an addon sends, the head stays far below the server's 16 KiB cap
    std::vector<std::string> flood;
    for (int i = 0; i < 40; ++i) flood.push_back("tracker:udp://" + std::string(190, '&') + std::to_string(i));
    url = localStreamUrl(H, 0, flood, std::string(300, '&'));
    CHECK(parseLocalStreamUrl(url, r) && url.size() < 12000);

    // other URLs aren't local streams
    CHECK(!parseLocalStreamUrl("https://download.blender.org/durian/movies/Sintel.2010.1080p.mkv", r));
    CHECK(!parseLocalStreamUrl("http://127.0.0.1:11471/" + H + "/0", r));
    CHECK(!parseLocalStreamUrl("http://127.0.0.1:11470/nothex/0", r));
    CHECK(!parseLocalStreamUrl("", r));

    // the downloader's extension comes from the name hint
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "Movie.2020.MKV")), "mkv");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "Sintel.2010.720p.BluRay.x264.AAC-[YTS.MX].mp4")), "mp4");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "Season 1/E01.avi")), "avi");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "noext")), "");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "dir.v2/noext")), "");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "odd.m?v")), "");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "long.extension")), "");
    CHECK_EQ(localStreamExtension(localStreamUrl(H, 0, {}, "")), "");
    CHECK_EQ(localStreamExtension("https://example.com/a.mkv"), "");

    // seeders as addons print them
    CHECK(parseSeeders("Sintel 2010 720p BluRay\n\xF0\x9F\x91\xA4 14 \xF0\x9F\x92\xBE 136.47 MB") == 14);
    CHECK(parseSeeders("\xF0\x9F\x91\xA5 3") == 3);
    CHECK(parseSeeders("\xF0\x9F\x91\xA4: 250") == 250);
    CHECK(parseSeeders("Seeders: 45") == 45);
    CHECK(parseSeeders("45 seeders") == 45);
    CHECK(parseSeeders("Seed 7") == 7);
    CHECK(parseSeeders("\xF0\x9F\x91\xA4 0") == 0);
    CHECK(parseSeeders("Sintel 1080p") == -1);
    CHECK(parseSeeders("\xF0\x9F\x91\xA4 n/a") == -1);
    CHECK(parseSeeders("") == -1);

    // same quality: a link before a torrent, better seeded torrents first, unknown last
    CHECK(torrentOrder(false, -1, true, 100) < 0);
    CHECK(torrentOrder(true, 100, false, -1) > 0);
    CHECK(torrentOrder(true, 50, true, 5) < 0);
    CHECK(torrentOrder(true, 5, true, 50) > 0);
    CHECK(torrentOrder(true, 0, true, -1) < 0);
    CHECK(torrentOrder(true, 7, true, 7) == 0);
    CHECK(torrentOrder(false, -1, false, -1) == 0);

    // mpv waits longer than the engine's stall timeout, so the engine's error is the one shown
    CHECK(playerNetworkTimeoutSecs > EngineSettings{}.stallSecs);
    // and never retries a body the server ended (each retry of a stalled torrent waited the
    // stall timeout again): mpv's options for a local stream URL
    CHECK(playerOptions() == "network-timeout=120,stream-lavf-o=reconnect=0");

    if (failures == 0) printf("test_torrent_source: all passed\n");
    return failures ? 1 : 0;
}
