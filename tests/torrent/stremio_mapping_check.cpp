// Laptop check: torrent streams from an addon's JSON become playable source rows whose part
// key is the local server's URL (USE_TORRENT and the server ready), and stay grey rows
// otherwise. It needs GMCA's Stremio headers with curl's and borealis', so it builds in the
// desktop image (tests/torrent/desktop.Dockerfile), from the repo root:
//   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src gmca-desktop-torrent bash -c
//     'g++ -std=gnu++17 -Wall -DUSE_TORRENT -Iapp/include -Ilibrary/borealis/library/include
//      -Ilibrary/borealis/library/include/borealis/extern tests/torrent/stremio_mapping_check.cpp
//      -o /tmp/m && /tmp/m tests/torrent/fake-addon/stream/movie/tt1727587.json'
// `/tmp/m <file> --url N` prints only stream N's local URL, for curl.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <api/stremio/types.hpp>

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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <stream json> [--url N]\n", argv[0]);
        return 2;
    }
    std::ifstream file(argv[1]);
    std::vector<stremio::StreamOption> streams = stremio::parseStreams(nlohmann::json::parse(file));

    if (argc == 4 && std::string(argv[2]) == "--url") {
        size_t n = size_t(std::atoi(argv[3]));
        if (n >= streams.size()) return 2;
        media::Media m = stremio::streamToMedia(streams[n], "Fake torrents", true);
        if (!m.playable()) return 1;
        std::printf("%s\n", m.parts.front().key.c_str());
        return 0;
    }

    CHECK(streams.size() == 3);
    if (streams.size() != 3) return 1;

    // WebTorrent's Sintel, with trackers and a file name
    media::Media a = stremio::streamToMedia(streams[0], "Fake torrents", true);
    CHECK(a.kind == media::SourceKind::Torrent && a.playable());
    CHECK_EQ(a.parts.front().key,
             "http://127.0.0.1:11470/08ada5a7a6183aae1e09d831df6748d566095a10/5"
             "?tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce&tr=udp%3A%2F%2Fexplodie.org%3A6969&f=Sintel.mp4");
    CHECK(a.seeders == 12);
    CHECK_EQ(a.detail, "129.2 MB");
    CHECK_EQ(a.videoResolution, "720p");
    CHECK_EQ(a.label, "Fake torrents");

    // Torrentio's shape: an uppercase hash, no sources
    media::Media b = stremio::streamToMedia(streams[1], "Fake torrents", true);
    CHECK(b.kind == media::SourceKind::Torrent && b.playable());
    CHECK_EQ(b.parts.front().key, "http://127.0.0.1:11470/a779fa758f2229586eebc2e5960eb32fe5659596/0"
                                  "?f=Sintel.2010.720p.BluRay.x264.AAC-%5BYTS.MX%5D.mp4");
    CHECK(b.seeders == 14);
    CHECK_EQ(b.detail, "H.264  \xC2\xB7  136.47 MB");

    // a link is unchanged
    media::Media c = stremio::streamToMedia(streams[2], "Fake torrents", true);
    CHECK(c.kind == media::SourceKind::Direct && c.playable());
    CHECK_EQ(c.parts.front().key, "https://download.blender.org/durian/movies/Sintel.2010.1080p.mkv");

    // the server not ready (switched off, port taken): torrents stay grey rows
    media::Media off = stremio::streamToMedia(streams[0], "Fake torrents", false);
    CHECK(off.kind == media::SourceKind::Torrent && !off.playable() && off.parts.empty());

    if (failures == 0) printf("stremio_mapping_check: all passed\n");
    return failures ? 1 : 0;
}
