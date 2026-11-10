// Standalone logic test: what the app shows about torrents (failure messages; the loading
// screen's stages, progress and pulse).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_ui.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <torrent/ui_model.hpp>

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

    // why torrent rows are grey
    CHECK_EQ(availabilityKey(Availability::Off), "main/torrent/error/off");
    CHECK_EQ(availabilityKey(Availability::NotStarted), "main/torrent/error/start");
    CHECK_EQ(availabilityKey(Availability::Ready), "main/torrent/error/unplayable");
    CHECK_EQ(availabilityKey(Availability::Stopped), "main/torrent/error/unplayable");

    // why a torrent didn't load (a cancel says nothing)
    CHECK_EQ(errorKey(Status::NoPeers), "main/torrent/error/no_peers");
    CHECK_EQ(errorKey(Status::DiskError), "main/torrent/error/no_space");
    CHECK_EQ(errorKey(Status::Failed), "main/torrent/error/unplayable");
    CHECK_EQ(errorKey(Status::NotFound), "main/torrent/error/unplayable");
    CHECK_EQ(errorKey(Status::BadRequest), "main/torrent/error/unplayable");
    CHECK_EQ(errorKey(Status::Stopped), "");
    CHECK_EQ(errorKey(Status::Ok), "");

    // mpv's error on a torrent: the engine's error first, then the server's status
    CHECK_EQ(playerErrorKey("mpv -13: loading failed: http: HTTP error 504 Gateway Timeout", Status::Ok),
             "main/torrent/error/no_peers");
    CHECK_EQ(playerErrorKey("mpv -13: loading failed: http: HTTP error 507 Insufficient Storage", Status::Ok),
             "main/torrent/error/no_space");
    CHECK_EQ(playerErrorKey("mpv -13: loading failed: http: HTTP error 503 Service Unavailable", Status::Ok),
             "main/torrent/error/off");
    CHECK_EQ(playerErrorKey("mpv -13: loading failed: http: HTTP error 504 Gateway Timeout", Status::DiskError),
             "main/torrent/error/no_space");
    CHECK_EQ(playerErrorKey("mpv -17: unrecognized file format", Status::Failed), "main/torrent/error/unplayable");
    CHECK_EQ(playerErrorKey("mpv -17: unrecognized file format", Status::Ok), "main/torrent/error/unplayable");
    CHECK_EQ(playerErrorKey("", Status::NoPeers), "main/torrent/error/unplayable");

    // a torrent stream that broke off during playback: mpv sees the end of the file early
    CHECK(endedEarly(888, 400.2));
    CHECK(endedEarly(888, 882.9));
    CHECK(!endedEarly(888, 887.96));  // it played to its end
    CHECK(!endedEarly(888, 884));     // within a few seconds of it
    CHECK(!endedEarly(0, 12));        // no duration known: nothing to say
    CHECK(!endedEarly(-1, 12));
    // and why: the engine's error, else no peers (a stall ends a stream without one)
    CHECK_EQ(endErrorKey(Status::DiskError), "main/torrent/error/no_space");
    CHECK_EQ(endErrorKey(Status::Failed), "main/torrent/error/unplayable");
    CHECK_EQ(endErrorKey(Status::Ok), "main/torrent/error/no_peers");
    CHECK_EQ(endErrorKey(Status::NoPeers), "main/torrent/error/no_peers");
    CHECK_EQ(endErrorKey(Status::Stopped), "main/torrent/error/no_peers");

    // the service's snapshot keeps the error the engine last reported for the watched torrent
    // once the engine has dropped it (30 s after its last reader, maybe before mpv's end)
    Stats full;
    full.known = true;
    full.error = Status::DiskError;
    Stats dropped;  // the engine no longer has it
    Stats kept = keepLastError(dropped, full);
    CHECK(!kept.known && kept.error == Status::DiskError);
    CHECK(keepLastError(dropped, kept).error == Status::DiskError);
    Stats again;  // added again: what the engine says of it now
    again.known = true;
    CHECK(keepLastError(again, kept).error == Status::Ok);
    CHECK(keepLastError(dropped, Stats{}).error == Status::Ok);

    // the loading screen's stages and bar
    Stats st;
    LoadingState l = loadingState(st, 0, 0);
    CHECK(l.stage == LoadingStage::FindingPeers && l.percent == 0 && l.progress == 0.05f);
    st.peers = 12;
    st.downloadBytesPerSec = 3565158;
    l = loadingState(st, 0, 0);
    CHECK(l.stage == LoadingStage::GettingInfo && l.progress == 0.10f && l.peers == 12 && l.bytesPerSec == 3565158);
    st.hasMetadata = true;  // the engine has it, the warm-up doesn't know the file yet
    l = loadingState(st, 0, 0);
    CHECK(l.stage == LoadingStage::Buffering && l.percent == 0 && l.progress == 0.10f);
    l = loadingState(st, 1887437, 4194304);  // 45%
    CHECK(l.stage == LoadingStage::Buffering && l.percent == 45);
    CHECK(l.progress > 0.50f && l.progress < 0.51f);
    // all of it in: still 99% and a bar short of full, since 100% would read as playing and the
    // screen goes only when mpv has data
    l = loadingState(st, 4194304, 4194304);
    CHECK(l.percent == 99 && l.progress < 1.0f && l.progress > 0.99f);
    l = loadingState(st, 4173824, 4194304);  // 99.5%
    CHECK(l.percent == 99);
    st.hasMetadata = false;  // the warm-up's target says the metadata is in, whatever stats lag
    l = loadingState(st, 1048576, 4194304);
    CHECK(l.stage == LoadingStage::Buffering && l.percent == 25);
    l = loadingState(st, 5000000, 4194304);  // never past the end
    CHECK(l.percent == 99 && l.progress < 1.0f);
    l = loadingState(st, 0, 123);  // a file smaller than the buffer
    CHECK(l.stage == LoadingStage::Buffering && l.percent == 0);

    // "12 peers · 3.4 MB/s"
    CHECK_EQ(formatRate(3565158), "3.4 MB/s");
    CHECK_EQ(formatRate(1 << 20), "1.0 MB/s");
    CHECK_EQ(formatRate(524288), "512 KB/s");
    CHECK_EQ(formatRate(1000), "0 KB/s");
    CHECK_EQ(formatRate(0), "0 KB/s");
    CHECK_EQ(formatRate(-5), "0 KB/s");

    // the slow pulse: full at 0, faintest half way, full again after 2.4 s
    CHECK(pulseAlpha(0) > 0.999f);
    CHECK(pulseAlpha(1.2) > 0.449f && pulseAlpha(1.2) < 0.451f);
    CHECK(pulseAlpha(2.4) > 0.999f);
    CHECK(pulseAlpha(0.6) < pulseAlpha(0.3) && pulseAlpha(1.8) > pulseAlpha(1.5));
    CHECK(pulseAlpha(-1) > 0.999f);

    // the focused torrent row's pill: live peers once the engine has some, else the seeders
    {
        Stats live;
        live.known = true;
        live.peers = 23;
        CHECK_EQ(rowPeers(45, live).key, "main/torrent/loading/peers");
        CHECK(rowPeers(45, live).count == 23);
        CHECK(rowPeers(-1, live).count == 23);
        live.peers = 0;  // the engine has it, still connecting
        CHECK_EQ(rowPeers(45, live).key, "main/torrent/source/seeders");
        CHECK(rowPeers(45, live).count == 45);
        CHECK_EQ(rowPeers(45, Stats{}).key, "main/torrent/source/seeders");
        CHECK_EQ(rowPeers(-1, Stats{}).key, "");
    }

    if (failures == 0) printf("test_torrent_ui: all passed\n");
    return failures ? 1 : 0;
}
