// Standalone logic test: Stremio-style stream URLs and the HTTP pieces of the torrent server.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_http.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include <torrent/byte_range.hpp>
#include <torrent/http_head.hpp>
#include <torrent/stream_url.hpp>
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
    const std::string H = "01c137287d6f0ed05a56742dae794f632c79ff3d";

    StreamRequest r;
    CHECK(parseStreamPath("/" + H + "/0", r));
    CHECK_EQ(r.infoHash, H);
    CHECK(r.fileIndex == 0);
    CHECK(r.trackers.empty());

    CHECK(parseStreamPath("/01C137287D6F0ED05A56742DAE794F632C79FF3D/-1", r));
    CHECK_EQ(r.infoHash, H);
    CHECK(r.fileIndex == -1);

    CHECK(parseStreamPath("/" + H + "/3?tr=udp%3A%2F%2Ftracker.example%3A1337%2Fannounce&f=Movie.mkv&x=1", r));
    CHECK(r.fileIndex == 3);
    CHECK(r.trackers.size() == 1);
    CHECK_EQ(r.trackers[0], "udp://tracker.example:1337/announce");
    CHECK(r.fileHints.size() == 1);
    CHECK_EQ(r.fileHints[0], "Movie.mkv");

    CHECK(!parseStreamPath("/" + H, r));
    CHECK(!parseStreamPath("/" + H + "/", r));
    CHECK(!parseStreamPath("/" + H + "/1/", r));
    CHECK(!parseStreamPath("/" + H + "/-2", r));
    CHECK(!parseStreamPath("/" + H + "/1234567", r));
    CHECK(!parseStreamPath("/" + H.substr(1) + "/0", r));
    CHECK(!parseStreamPath("/" + H.substr(1) + "g/0", r));
    CHECK(!parseStreamPath(H + "/0", r));

    StreamRequest built;
    built.infoHash = H;
    built.fileIndex = 2;
    built.trackers = {"udp://t.example:451/announce"};
    built.fileHints = {"A B.mkv"};
    CHECK_EQ(streamPath(built), "/" + H + "/2?tr=udp%3A%2F%2Ft.example%3A451%2Fannounce&f=A%20B.mkv");
    StreamRequest back;
    CHECK(parseStreamPath(streamPath(built), back));
    CHECK(back.trackers.size() == 1 && back.trackers[0] == built.trackers[0]);
    CHECK(back.fileHints.size() == 1 && back.fileHints[0] == "A B.mkv");
    CHECK_EQ(magnetUri(built), "magnet:?xt=urn:btih:" + H + "&tr=udp%3A%2F%2Ft.example%3A451%2Fannounce");

    RequestHead h;
    CHECK(parseRequestHead("GET /" + H + "/0 HTTP/1.1\r\nHost: 127.0.0.1:11470\r\nrange:  bytes=100- \r\n"
                           "User-Agent: mpv\r\n\r\n",
        h));
    CHECK_EQ(h.method, "GET");
    CHECK_EQ(h.target, "/" + H + "/0");
    CHECK_EQ(h.range, "bytes=100-");
    CHECK(parseRequestHead("HEAD /x HTTP/1.0\r\n\r\n", h));
    CHECK_EQ(h.method, "HEAD");
    CHECK_EQ(h.range, "");
    CHECK(!parseRequestHead("GET /x\r\n\r\n", h));
    CHECK(!parseRequestHead("garbage", h));

    CHECK_EQ(statusLine(206), "HTTP/1.1 206 Partial Content");
    CHECK_EQ(statusLine(504), "HTTP/1.1 504 Gateway Timeout");
    CHECK_EQ(statusLine(507), "HTTP/1.1 507 Insufficient Storage");
    CHECK_EQ(statusLine(502), "HTTP/1.1 502 Bad Gateway");
    CHECK_EQ(statusName(Status::DiskError), "disk full");
    CHECK_EQ(statusName(Status::Failed), "torrent error");

    // the engine takes lowercase hashes only: an uppercase one would name a folder it never deletes
    CHECK(isInfoHash(H));
    CHECK(!isInfoHash("01C137287D6F0ED05A56742DAE794F632C79FF3D"));
    CHECK(!isInfoHash(H.substr(1)));
    CHECK(!isInfoHash(H + "0"));
    CHECK(!isInfoHash(H.substr(1) + "g"));
    CHECK(!isInfoHash(""));
    CHECK_EQ(contentTypeFor("Show/S01E01.MKV"), "video/x-matroska");
    CHECK_EQ(contentTypeFor("a.mp4"), "video/mp4");
    CHECK_EQ(contentTypeFor("noext"), "application/octet-stream");

    ByteRange b;
    CHECK(parseRange("", 1000, b) == RangeKind::Full && b.first == 0 && b.last == 999);
    CHECK(parseRange("bytes=0-99", 1000, b) == RangeKind::Partial && b.first == 0 && b.last == 99);
    CHECK(parseRange("bytes=900-", 1000, b) == RangeKind::Partial && b.first == 900 && b.last == 999);
    CHECK(parseRange("bytes=-100", 1000, b) == RangeKind::Partial && b.first == 900 && b.last == 999);
    CHECK(parseRange("bytes=-5000", 1000, b) == RangeKind::Partial && b.first == 0 && b.last == 999);
    CHECK(parseRange("bytes=500-99999", 1000, b) == RangeKind::Partial && b.first == 500 && b.last == 999);
    CHECK(parseRange("bytes=1000-", 1000, b) == RangeKind::Unsatisfiable);
    CHECK(parseRange("bytes=-0", 1000, b) == RangeKind::Unsatisfiable);
    CHECK(parseRange("bytes=5-2", 1000, b) == RangeKind::Full && b.first == 0 && b.last == 999);
    CHECK(parseRange("bytes=0-1,5-9", 1000, b) == RangeKind::Full);
    CHECK(parseRange("bytes=abc-", 1000, b) == RangeKind::Full);
    CHECK(parseRange("items=0-1", 1000, b) == RangeKind::Full);
    CHECK(parseRange("bytes=6655614976-6655619071", 6655619072ll, b) == RangeKind::Partial &&
          b.first == 6655614976ll && b.last == 6655619071ll);

    if (failures == 0) printf("test_torrent_http: all passed\n");
    return failures ? 1 : 0;
}
