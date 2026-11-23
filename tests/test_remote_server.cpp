// Standalone logic test: a Stremio streaming server set in Settings (torrent/remote_server.hpp):
// the address as typed, the create call's body, and the file it streams.
//
//   c++ -std=gnu++17 -Wall -Iapp/include -Ilibrary/borealis/library/include/borealis/extern tests/test_remote_server.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>

#include <torrent/remote_server.hpp>

using namespace torrent;
using namespace torrent::remote;

static int failures = 0;
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    // the address as typed
    CHECK_EQ(normalizeServerUrl("http://192.168.1.216:11470"), "http://192.168.1.216:11470");
    CHECK_EQ(normalizeServerUrl("  192.168.1.216:11470/ "), "http://192.168.1.216:11470");
    CHECK_EQ(normalizeServerUrl("HTTPS://stremio.lan/"), "https://stremio.lan");
    CHECK_EQ(normalizeServerUrl("http://acer:11470//"), "http://acer:11470");
    CHECK_EQ(normalizeServerUrl(""), "");
    CHECK_EQ(normalizeServerUrl("   "), "");
    CHECK_EQ(normalizeServerUrl("ftp://192.168.1.216"), "");
    CHECK_EQ(normalizeServerUrl("http://"), "");
    CHECK_EQ(normalizeServerUrl("http://:11470"), "");
    CHECK_EQ(normalizeServerUrl("http://my server:11470"), "");

    // the create call: trackers as sources, DHT, a file guess only without a file
    StreamRequest r;
    r.infoHash = "d160b8d8ea35a5b4e52837468fc8f03d55cef1f7";
    r.fileIndex = 2;
    r.trackers = {"udp://tracker.example:1337/announce"};
    CHECK_EQ(createBody(r).dump(),
        R"({"peerSearch":{"max":150,"min":40,"sources":["tracker:udp://tracker.example:1337/announce","dht:d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"]},"torrent":{"infoHash":"d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"}})");
    r.fileIndex = -1;
    r.trackers.clear();
    CHECK_EQ(createBody(r).dump(),
        R"({"guessFileIdx":{},"peerSearch":{"max":150,"min":40,"sources":["dht:d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"]},"torrent":{"infoHash":"d160b8d8ea35a5b4e52837468fc8f03d55cef1f7"}})");

    // the file: the stream's own, the server's guess, the name hint, the largest
    const auto stats = nlohmann::json::parse(R"({"guessedFileIdx":-1,"files":[
        {"name":"Show/Sample.mkv","length":20000000},
        {"name":"Show/Show.S01E02.mkv","length":900000000},
        {"name":"Show/Show.S01E01.mkv","length":800000000}]})");
    auto pick = [](const StreamRequest& q, const nlohmann::json& s) { return std::to_string(chooseFile(q, s)); };
    StreamRequest q;
    q.infoHash = r.infoHash;
    q.fileIndex = 2;
    CHECK_EQ(pick(q, stats), "2");
    q.fileIndex = -1;
    CHECK_EQ(pick(q, stats), "1");  // no guess, no hint: the largest
    q.fileHints = {"show.s01e01.MKV"};
    CHECK_EQ(pick(q, stats), "2");  // the hint, by name, any case
    q.fileHints = {"Other.mkv"};
    CHECK_EQ(pick(q, stats), "1");  // a hint no file has: the largest
    auto guessed = stats;
    guessed["guessedFileIdx"] = 0;
    CHECK_EQ(pick(q, guessed), "0");  // the server's guess wins over the hint and the size
    CHECK_EQ(pick(q, nlohmann::json::parse(R"({"files":null})")), "-1");  // no metadata yet
    CHECK_EQ(pick(q, nlohmann::json::parse(R"({"files":[]})")), "-1");
    CHECK_EQ(pick(q, nlohmann::json::object()), "-1");

    CHECK_EQ(streamUrl("http://192.168.1.216:11470", r.infoHash, 0),
        "http://192.168.1.216:11470/d160b8d8ea35a5b4e52837468fc8f03d55cef1f7/0");

    if (failures == 0) printf("test_remote_server: all passed\n");
    return failures ? 1 : 0;
}
