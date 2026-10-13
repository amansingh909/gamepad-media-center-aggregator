// Laptop checks for what the app needs from the torrent engine, the server and
// TorrentService. Offline checks use dead info hashes and need no torrent data;
// --online also saves the DHT state (once the DHT has found nodes) and buffers the start of
// Sintel (Blender Foundation, CC BY). Build and run, from
// the repo root, in the harness image (tests/torrent/Dockerfile):
//   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src gmca-torrent-harness
//     bash -c 'cmake -S tests/torrent -B build-torrent && cmake --build build-torrent -j$(nproc)'
//   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//     gmca-torrent-harness build-torrent/app_checks [--online] [--swarm]
// With --online, `--full-disk DIR` also fills a small tmpfs (about a minute more):
//   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//     --tmpfs /tmp/full:size=8m,uid=$(id -u),gid=$(id -g)
//     gmca-torrent-harness build-torrent/app_checks --online --full-disk /tmp/full
// --swarm also streams from a swarm of 40 local peers (local_swarm.hpp), no internet needed:
// about 5 minutes more (5.5 in all on the laptop), and 2 GiB of space in the container's /tmp.
// The checks listen on ports 11481 to 11486 of the container's own 127.0.0.1, the swarm's
// peers on 127.0.0.11 to 127.0.0.50.

#include <torrent/stream_url.hpp>
#include <torrent/torrent_engine.hpp>
#include <torrent/torrent_server.hpp>
#include <torrent/torrent_service.hpp>
#include <torrent/ui_model.hpp>

#include "local_swarm.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

// what the engine and the server logged, for the checks that read it
static std::mutex logMutex;
static std::vector<std::string> logLines;
static void logLine(const std::string& m) {
    std::lock_guard<std::mutex> lock(logMutex);
    logLines.push_back(m);
    std::printf("  log: %s\n", m.c_str());
    std::fflush(stdout);
}
static bool logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const auto& l : logLines)
        if (l.find(text) != std::string::npos) return true;
    return false;
}
static int countLogged(const std::string& text) {
    std::lock_guard<std::mutex> lock(logMutex);
    int n = 0;
    for (const auto& l : logLines)
        if (l.find(text) != std::string::npos) ++n;
    return n;
}
static long fileSize(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}
static std::string readFile(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
static void writeFile(const std::string& path, const std::string& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

// One HTTP/1.1 request to 127.0.0.1:port; the whole response, "" if it can't connect.
static std::string httpRequest(int port, const std::string& method, const std::string& target) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        close(fd);
        return "";
    }
    std::string req = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, req.data(), req.size(), 0);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof buf, 0)) > 0) out.append(buf, size_t(n));
    close(fd);
    return out;
}

static bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

static torrent::StreamRequest request(const std::string& hash, int index = 0) {
    torrent::StreamRequest r;
    r.infoHash = hash;
    r.fileIndex = index;
    return r;
}

// info hashes no swarm has
static std::string dead(int n) {
    std::string h = "00000000000000000000000000000000000000";
    char tail[3];
    std::snprintf(tail, sizeof tail, "%02x", n);
    return h + tail;
}

static torrent::EngineSettings engineSettings(const std::string& dir) {
    torrent::EngineSettings s;
    s.tempDir = dir;
    s.log = logLine;
    return s;
}

static void serverChecks() {
    std::printf("server\n");
    torrent::TorrentEngine engine(engineSettings("/tmp/app-checks/server"));
    torrent::TorrentServer server(engine, logLine);
    std::string error;
    CHECK(server.start(11481, false, error));
    CHECK(!server.start(11481, false, error) && error == "server already running");

    // the Settings switch off: 503 before the engine sees the request
    server.setEnabled(false);
    std::string r = httpRequest(11481, "GET", "/" + dead(1) + "/0");
    CHECK(startsWith(r, "HTTP/1.1 503") && r.find("Torrent streaming is off") != std::string::npos);
    CHECK(!engine.stats(dead(1)).known);
    server.setEnabled(true);

    // a wrong URL or method leaves a line in the log
    r = httpRequest(11481, "GET", "/nothex/0");
    CHECK(startsWith(r, "HTTP/1.1 400") && logged("400 for /nothex/0"));
    r = httpRequest(11481, "POST", "/" + dead(1) + "/0");
    CHECK(startsWith(r, "HTTP/1.1 405") && r.find("Allow: GET, HEAD") != std::string::npos && logged("405 for POST"));

    server.stop();
    CHECK(!server.start(11481, false, error) && error == "server stopped");
}

static void engineChecks() {
    std::printf("engine\n");
    torrent::TorrentEngine engine(engineSettings("/tmp/app-checks/engine"));

    // an uppercase hash would make a folder the engine never deletes: refused
    const std::string upper = "ABCDEF00000000000000000000000000000000A1";
    engine.prefetch(request(upper));
    CHECK(!engine.stats(upper).known && logged("bad info hash"));

    // a torrent the engine doesn't have: nothing known, no error
    torrent::Stats none = engine.stats(dead(2));
    CHECK(!none.known && none.error == torrent::Status::Ok && none.peers == 0);

    // a fourth prefetch nobody opened drops the oldest
    for (int n = 2; n <= 5; ++n) {
        engine.prefetch(request(dead(n)));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));  // the engine expires every quarter second
    CHECK(!engine.stats(dead(2)).known);
    CHECK(engine.stats(dead(3)).known && engine.stats(dead(4)).known && engine.stats(dead(5)).known);
    torrent::Stats waiting = engine.stats(dead(5));
    CHECK(!waiting.hasMetadata && waiting.error == torrent::Status::Ok);

    // a prefetch of a torrent in its grace period keeps it for the prefetch window: a replay
    // right after stopping finds it there
    {
        torrent::EngineSettings s = engineSettings("/tmp/app-checks/grace");
        s.graceSecs = 2;
        torrent::TorrentEngine graceEngine(s);
        torrent::FileInfo file;
        torrent::ReaderId reader = 0;
        // a reader that gives up at once: the torrent stays for the 2 s grace period
        CHECK(graceEngine.acquire(request(dead(20)), file, reader, [] { return true; }) == torrent::Status::Stopped);
        CHECK(graceEngine.acquire(request(dead(21)), file, reader, [] { return true; }) == torrent::Status::Stopped);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        graceEngine.prefetch(request(dead(20)));
        std::this_thread::sleep_for(std::chrono::seconds(3));  // past the grace period
        CHECK(graceEngine.stats(dead(20)).known && logged("prefetch " + dead(20) + ": kept 120 s"));
        CHECK(!graceEngine.stats(dead(21)).known);  // nobody looked at this one again: gone
    }
}

// A DHT state file as libtorrent writes it, listing these IPv4 nodes.
static std::string dhtState(const std::vector<std::string>& nodes) {
    auto bytes = [](const std::string& s) { return std::to_string(s.size()) + ":" + s; };
    std::string list;
    for (const auto& n : nodes) list += bytes(n);
    return "d" + bytes("dht state") + "d" + bytes("nodes") + "l" + list + "eee";
}
// One node as the file lists it: the address's 4 bytes, then the port's 2.
static std::string node4(int a, int b, int c, int d, int port) {
    return std::string{char(a), char(b), char(c), char(d), char(port >> 8), char(port & 0xff)};
}

// An engine whose session starts with `s` and closes at once, before its DHT knows any node.
static void briefSession(const torrent::EngineSettings& s) {
    torrent::TorrentEngine engine(s);
    engine.keepSessionOpen(true);
}

// The DHT state is loaded when a session starts, a damaged file is ignored, and a DHT that
// knows no node yet (a session that closes right after it starts, as here) never replaces the
// file: the last good copy stays. Saving needs a DHT that has found nodes, on the internet:
// dhtOnlineChecks(). A session kept open doesn't close for idleness.
static void stateChecks() {
    std::printf("dht state\n");
    const std::string file = "/tmp/app-checks/torrent-dht.dat";
    std::remove(file.c_str());
    torrent::EngineSettings s = engineSettings("/tmp/app-checks/state");
    s.stateFile = file;
    const std::string notSaved = "DHT state not saved: no nodes";
    const int sessions = countLogged("session started");
    const int skipped = countLogged(notSaved);

    // a first run: no file, and none made from an empty DHT
    briefSession(s);
    CHECK(countLogged("session started") == sessions + 1 && countLogged(notSaved) == skipped + 1);
    CHECK(fileSize(file) == -1 && !logged("loaded the DHT state"));

    // a good file (two documentation addresses, nobody answers): loaded, and kept as it is
    const std::string good = dhtState({node4(192, 0, 2, 1, 6881), node4(192, 0, 2, 2, 6881)});
    writeFile(file, good);
    briefSession(s);
    CHECK(logged("loaded the DHT state: 2 nodes") && countLogged(notSaved) == skipped + 2);
    CHECK(readFile(file) == good);

    // a damaged file: the session starts without it; only a DHT with nodes replaces it
    const std::string damaged = "this is not a DHT state";
    writeFile(file, damaged);
    briefSession(s);
    CHECK(logged("DHT state unreadable") && countLogged("session started") == sessions + 3);
    CHECK(countLogged(notSaved) == skipped + 3 && readFile(file) == damaged);

    torrent::EngineSettings idle = engineSettings("/tmp/app-checks/state");
    idle.idleSecs = 1;
    torrent::TorrentEngine engine(idle);
    const int closes = countLogged("idle, closing the session");
    engine.keepSessionOpen(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(countLogged("idle, closing the session") == closes);
    engine.keepSessionOpen(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(countLogged("idle, closing the session") == closes + 1);
}

// What a warm-up's `done` was called with.
struct Calls {
    std::mutex m;
    int n = 0;
    torrent::Status lastStatus = torrent::Status::Ok;
    std::function<void(torrent::Status)> recorder() {
        return [this](torrent::Status s) {
            std::lock_guard<std::mutex> lock(m);
            ++n;
            lastStatus = s;
        };
    }
    int count() {
        std::lock_guard<std::mutex> lock(m);
        return n;
    }
    torrent::Status last() {
        std::lock_guard<std::mutex> lock(m);
        return lastStatus;
    }
};

static torrent::ServiceSettings serviceSettings(const std::string& dir, uint16_t port) {
    torrent::ServiceSettings s;
    s.tempDir = dir;
    s.port = port;
    s.log = logLine;
    return s;
}

static void serviceChecks() {
    std::printf("service\n");
    using Clock = std::chrono::steady_clock;

    // a port someone else holds: no server, and the reason for the grey rows
    {
        int blocker = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(11482);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(bind(blocker, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(blocker, 1) == 0);
        torrent::TorrentService busy(serviceSettings("/tmp/app-checks/busy", 11482));
        CHECK(!busy.start());
        CHECK(busy.availability() == torrent::Availability::NotStarted);
        CHECK(busy.startError().find("11482") != std::string::npos);
        close(blocker);
    }

    torrent::TorrentService service(serviceSettings("/tmp/app-checks/service", 11483));
    CHECK(service.availability() == torrent::Availability::NotStarted);
    const int kept = countLogged("keeping the session open");
    const int idling = countLogged("the session closes when idle");
    CHECK(service.start());
    CHECK(service.availability() == torrent::Availability::Ready);
    // the session starts with the service, on its worker, and stays open while streaming is on
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(countLogged("keeping the session open") == kept + 1);
    // "Allow incoming connections": applied by the worker, at once, to the running session
    service.setPortMapping(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(logged("incoming connections on (UPnP, NAT-PMP)"));
    service.setPortMapping(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(logged("incoming connections off"));

    // the Settings switch: off refuses streams and prefetches, on serves again
    service.setEnabled(false);
    CHECK(service.availability() == torrent::Availability::Off);
    CHECK(startsWith(httpRequest(11483, "GET", "/" + dead(6) + "/0"), "HTTP/1.1 503"));
    service.prefetch(request(dead(6)));
    // the worker lets the session idle before the switch goes on again (it would take only the
    // last of two quick changes)
    for (int i = 0; i < 40 && countLogged("the session closes when idle") == idling; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    service.setEnabled(true);
    CHECK(service.availability() == torrent::Availability::Ready);

    // prefetch returns at once: the worker makes the engine call
    Clock::time_point t0 = Clock::now();
    service.prefetch(request(dead(7)));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(20));
    service.watch(dead(7));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(service.snapshot().known);
    // the switch went off and on meanwhile: the session may idle, then is kept open again
    CHECK(countLogged("the session closes when idle") == idling + 1);
    CHECK(countLogged("keeping the session open") == kept + 2);
    service.watch(dead(6));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(!service.snapshot().known);  // the prefetch made while off never reached the engine
    service.watch("");

    // a cancelled warm-up says Stopped, once, within about a second
    Calls cancelled;
    auto w = service.warmUp(request(dead(8)), torrent::startBufferBytes, cancelled.recorder());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(cancelled.count() == 0);  // still waiting for metadata that never comes
    CHECK(w->target() == 0 && w->buffered() == 0);
    w->cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(cancelled.count() == 1 && cancelled.last() == torrent::Status::Stopped);
    w->cancel();

    // dropping the handle without cancel() ends an unfinished warm-up the same way
    Calls dropped;
    {
        auto w3 = service.warmUp(request(dead(11)), torrent::startBufferBytes, dropped.recorder());
        std::this_thread::sleep_for(std::chrono::seconds(1));
        CHECK(dropped.count() == 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(dropped.count() == 1 && dropped.last() == torrent::Status::Stopped);

    // the player leaves a torrent before its start is in (another source or episode): the
    // warm-up gives up at once, it doesn't keep its reader until the bytes or the stall come
    Calls left;
    auto w4 = service.warmUp(request(dead(12)), torrent::startBufferBytes, left.recorder());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(left.count() == 0);
    torrent::endWarmup(*w4, /*ready=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(left.count() == 1 && left.last() == torrent::Status::Stopped);

    // stop() ends a waiting warm-up, and later ones are refused at once
    Calls pending;
    auto w2 = service.warmUp(request(dead(9)), torrent::startBufferBytes, pending.recorder());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    t0 = Clock::now();
    service.stop();
    CHECK(Clock::now() - t0 < std::chrono::seconds(10));
    CHECK(pending.count() == 1 && pending.last() == torrent::Status::Stopped);
    CHECK(service.availability() == torrent::Availability::Stopped);
    Calls late;
    service.warmUp(request(dead(10)), torrent::startBufferBytes, late.recorder());
    CHECK(late.count() == 1 && late.last() == torrent::Status::Stopped);
    service.stop();
}

// Saving the DHT state needs a DHT that has found nodes: a few seconds on the internet. Saved
// when an idle session closes (replacing a damaged file) and when the engine goes, loaded by the
// next session, never with no nodes; an unwritable file is only logged.
static void dhtOnlineChecks() {
    std::printf("dht state online\n");
    const std::string file = "/tmp/app-checks/torrent-dht-online.dat";
    const std::string damaged = "this is not a DHT state";
    writeFile(file, damaged);
    torrent::EngineSettings s = engineSettings("/tmp/app-checks/state-online");
    s.stateFile = file;
    const std::string saved = "saved the DHT state: ";
    const int saves = countLogged(saved);
    const int closes = countLogged("idle, closing the session");
    {
        torrent::EngineSettings idle = s;
        idle.idleSecs = 1;
        torrent::TorrentEngine engine(idle);
        engine.keepSessionOpen(true);
        std::this_thread::sleep_for(std::chrono::seconds(4));  // the DHT finds nodes
        engine.keepSessionOpen(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        // saved as the idle session closed, before the engine goes
        CHECK(countLogged("idle, closing the session") == closes + 1 && countLogged(saved) == saves + 1);
    }
    CHECK(countLogged(saved) == saves + 1);  // no session left to save
    CHECK(fileSize(file) > 0 && readFile(file) != damaged && !logged(saved + "0 nodes"));

    // the next session starts from those nodes, and saves again when the engine goes
    const int loads = countLogged("loaded the DHT state: ");
    {
        torrent::TorrentEngine engine(s);
        engine.keepSessionOpen(true);
        std::this_thread::sleep_for(std::chrono::seconds(4));
    }
    CHECK(countLogged("loaded the DHT state: ") == loads + 1 && !logged("loaded the DHT state: 0 nodes"));
    CHECK(countLogged(saved) == saves + 2 && !logged(saved + "0 nodes"));

    // a folder that doesn't exist: the save fails, it is logged, nothing else happens
    {
        torrent::EngineSettings u = s;
        u.stateFile = "/nonexistent-dir/torrent-dht.dat";
        torrent::TorrentEngine engine(u);
        engine.keepSessionOpen(true);
        std::this_thread::sleep_for(std::chrono::seconds(4));
    }
    CHECK(logged("saving the DHT state: cannot write /nonexistent-dir/torrent-dht.dat.part"));
}

// Sintel (Blender Foundation, CC BY), WebTorrent's sample torrent: file 5 is Sintel.mp4,
// 129241752 bytes. No tracker in the request: the engine's default trackers find peers.
static void onlineChecks() {
    std::printf("online\n");
    const std::string sintel = "08ada5a7a6183aae1e09d831df6748d566095a10";
    torrent::TorrentService service(serviceSettings("/tmp/app-checks/online", 11484));
    CHECK(service.start());
    service.watch(sintel);
    Calls calls;
    auto w = service.warmUp(request(sintel, 5), torrent::startBufferBytes, calls.recorder());
    // while it buffers, the loading screen's progress moves inside pieces too (their
    // written blocks), not only in buffered()'s 256 KiB steps, and never goes back
    int64_t highest = 0;
    int between = 0, offStep = 0;
    bool backwards = false;
    for (int i = 0; i < 2400 && calls.count() == 0; ++i) {
        const int64_t v = w->progress();
        if (v < highest) backwards = true;
        highest = std::max(highest, v);
        if (calls.count() == 0 && v > 0 && v < torrent::startBufferBytes) {  // read before Ok
            ++between;
            if (v % (256 << 10) != 0) ++offStep;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("  progress before Ok: %d samples between 0 and the target, %d off the 256 KiB steps\n", between,
                offStep);
    CHECK(calls.count() == 1 && calls.last() == torrent::Status::Ok);
    CHECK(between > 0 && offStep > 0 && !backwards);
    CHECK(w->target() == torrent::startBufferBytes && w->buffered() == torrent::startBufferBytes);
    CHECK(w->progress() == torrent::startBufferBytes);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    torrent::Stats s = service.snapshot();
    CHECK(s.known && s.hasMetadata && s.peers > 0 && s.error == torrent::Status::Ok);
    // while the warm-up holds its reader, the server answers for the same file
    std::string head = httpRequest(11484, "HEAD", "/" + sintel + "/5");
    CHECK(startsWith(head, "HTTP/1.1 200") && head.find("Content-Length: 129241752") != std::string::npos);
    w->finish();
    service.stop();
}

// A full disk: `dir` is a small tmpfs (8 MB), less than the 32 MiB a reader's window asks for.
// The engine fails the torrent with DiskError, and the service keeps reporting that error for
// the watched torrent after the engine has dropped it (30 s after its last reader): a player
// whose stream broke off asks why only once mpv has played its cache, which can be later.
static void fullDiskChecks(const std::string& dir) {
    std::printf("full disk\n");
    const std::string sintel = "08ada5a7a6183aae1e09d831df6748d566095a10";
    torrent::TorrentService service(serviceSettings(dir, 11485));
    CHECK(service.start());
    service.watch(sintel);
    Calls calls;
    auto w = service.warmUp(request(sintel, 5), torrent::startBufferBytes, calls.recorder());
    torrent::Status seen = torrent::Status::Ok;
    for (int i = 0; i < 360 && seen != torrent::Status::DiskError; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        seen = service.snapshot().error;
    }
    std::printf("  engine error %s, warm-up %s\n", torrent::statusName(seen),
                calls.count() ? torrent::statusName(calls.last()) : "still waiting");
    CHECK(seen == torrent::Status::DiskError && service.snapshot().known);
    // the warm-up's reader goes: the engine drops the torrent and deletes its folder
    w->cancel();
    using Clock = std::chrono::steady_clock;
    const Clock::time_point left = Clock::now();
    while (service.snapshot().known && Clock::now() - left < std::chrono::seconds(60))
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    torrent::Stats after = service.snapshot();
    std::printf("  dropped after %d s: known %d, error %s\n",
                int(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - left).count()), int(after.known),
                torrent::statusName(after.error));
    CHECK(!after.known && after.error == torrent::Status::DiskError);
    // watching it afresh starts with no error
    service.watch("");
    service.watch(sintel);
    CHECK(service.snapshot().error == torrent::Status::Ok);
    service.stop();
}

// What a player saw while it streamed a file from the engine.
struct Playback {
    bool ok = false;          // every read returned Ok, with the file's bytes, before the time limit
    double startSecs = 0;     // from the request to the first frame
    int stalls = 0;           // times the player ran out of data after it started
    double stalledSecs = 0;   // how long it waited in all, after it started
    int waits = 0;            // reads the engine held over 100 ms after the start
    double waitedSecs = 0;
    double playedSecs = 0;    // from the first frame to the last
    // the least the player had cached when the reader waited for data, once it had played 5 s
    // (it starts with what the warm-up and the first piece brought)
    double leastAheadSecs = 0;
    int64_t downloaded = 0;   // payload the seeders sent
    int64_t kept = 0;         // bytes of the file the engine has at the end
};

// A player streaming `playBytes` of a file at `bitrate` bytes per second, the way GMCA's mpv
// reads a local torrent URL: the server's 256 KiB reads, a cache that reads ahead as fast as
// the engine gives data, up to 60 s or 64 MiB past the playhead (demuxer-readahead-secs,
// demuxer-max-bytes); playback once the first 4 MiB are in (the warm-up's start buffer) and the
// file's last MiB has been read on a second connection (mpv reads a Matroska file's index there
// before it plays), and after a stall, playback again once 5 s are cached (cache-pause-wait).
static Playback play(torrent::TorrentEngine& engine, const torrent::StreamRequest& req, int64_t playBytes,
                     int64_t bitrate, const swarm::Seeders& seeders) {
    using Clock = std::chrono::steady_clock;
    auto secsSince = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    Playback out;
    const Clock::time_point begin = Clock::now();
    torrent::FileInfo file;
    torrent::ReaderId reader = 0;
    if (engine.acquire(req, file, reader) != torrent::Status::Ok) return out;
    playBytes = std::min(playBytes, file.size);
    const int64_t chunk = 256 << 10;
    const int64_t cacheAhead = std::min<int64_t>(64ll << 20, 60 * bitrate);
    const int64_t startBytes = 4ll << 20, resumeBytes = 5 * bitrate;

    std::mutex m;
    int64_t pos = 0;          // the reader's next byte
    double playhead = 0;      // the player's
    bool started = false, failed = false, tailIn = false;
    bool blocked = false;     // the reader waits in waitFor()
    Clock::time_point blockedSince = begin;
    std::thread tailThread([&] {
        torrent::FileInfo f;
        torrent::ReaderId tail = 0;
        bool ok = engine.acquire(req, f, tail) == torrent::Status::Ok;
        for (int64_t at = std::max<int64_t>(0, f.size - (1 << 20)); ok && at < f.size; at += chunk)
            ok = engine.waitFor(req, tail, at, std::min(at + chunk, f.size)) == torrent::Status::Ok;
        if (tail) engine.release(req, tail);
        std::lock_guard<std::mutex> lock(m);
        if (ok) tailIn = true;
        else failed = true;
    });
    std::thread readThread([&] {
        int fd = -1;
        std::vector<unsigned char> buf(size_t(chunk), 0);
        while (true) {
            int64_t at;
            bool full;
            {
                std::lock_guard<std::mutex> lock(m);
                if (pos >= playBytes || failed) break;
                at = pos;
                full = double(at) - playhead >= double(cacheAhead);
            }
            if (full) {  // the cache is full: the player reads nothing until it plays some
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const int64_t end = std::min(at + chunk, playBytes);
            const Clock::time_point t0 = Clock::now();
            {
                std::lock_guard<std::mutex> lock(m);
                blocked = true;
                blockedSince = t0;
            }
            const torrent::Status st = engine.waitFor(req, reader, at, end);
            const double waited = secsSince(t0);
            {
                std::lock_guard<std::mutex> lock(m);
                blocked = false;
            }
            if (st == torrent::Status::Ok && fd < 0) fd = open(file.diskPath.c_str(), O_RDONLY);
            bool right = st == torrent::Status::Ok && fd >= 0 &&
                         pread(fd, buf.data(), size_t(end - at), at) == ssize_t(end - at);
            for (int64_t i = 0; right && i < end - at; ++i) right = buf[size_t(i)] == swarm::byteAt(at + i);
            std::lock_guard<std::mutex> lock(m);
            if (!right) {
                std::printf("  read at %lld: %s\n", (long long)at, torrent::statusName(st));
                failed = true;
                break;
            }
            if (started && waited > 0.1) {
                ++out.waits;
                out.waitedSecs += waited;
            }
            pos = end;
        }
        if (fd >= 0) close(fd);
    });

    // the player, in 10 ms steps
    bool stalled = false;
    double leastAhead = 1e9;
    Clock::time_point last = Clock::now(), stallStart = last, startAt = last, nextLine = last;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const Clock::time_point now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        std::lock_guard<std::mutex> lock(m);
        if (failed || secsSince(begin) > 900) {
            failed = true;
            break;
        }
        if (now >= nextLine) {
            nextLine = now + std::chrono::seconds(5);
            std::printf("  t %5.1f s: player %6.1f MiB%s, reader %6.1f MiB, stalls %d (%.1f s), down %d KiB/s\n",
                        secsSince(begin), playhead / (1 << 20), !started ? " (starting)" : stalled ? " (stalled)" : "",
                        double(pos) / (1 << 20), out.stalls, out.stalledSecs,
                        engine.stats(req.infoHash).downloadBytesPerSec >> 10);
            std::fflush(stdout);
        }
        if (!started) {
            if (pos >= std::min(startBytes, playBytes) && tailIn) {
                started = true;
                startAt = now;
                out.startSecs = secsSince(begin);
            }
            continue;
        }
        if (stalled) {
            if (double(pos) - playhead >= double(resumeBytes) || pos >= playBytes) {
                stalled = false;
                out.stalledSecs += std::chrono::duration<double>(now - stallStart).count();
            }
            continue;
        }
        playhead = std::min(playhead + double(bitrate) * dt, double(playBytes));
        if (playhead >= double(playBytes)) break;
        if (playhead >= double(pos)) {
            playhead = double(pos);
            stalled = true;
            ++out.stalls;
            stallStart = now;
        }
        // what the player has cached when the reader waits for data, once it has played 5 s (it
        // starts with what the warm-up and the first piece brought)
        if (blocked && now - blockedSince > std::chrono::milliseconds(20) && playhead >= 5.0 * double(bitrate))
            leastAhead = std::min(leastAhead, (double(pos) - playhead) / double(bitrate));
    }
    {
        std::lock_guard<std::mutex> lock(m);
        out.ok = !failed;
        out.leastAheadSecs = leastAhead;
        out.playedSecs = secsSince(startAt);
    }
    readThread.join();
    tailThread.join();
    out.downloaded = seeders.uploaded();
    out.kept = engine.bytesReady(req, reader, 0, file.size);
    engine.release(req, reader);
    return out;
}

// Streaming a 4K-like file (4 MiB/s) of a torrent with big pieces from a swarm whose upload adds
// up to twice that, over many peers, none of them fast, most with part of the pieces: GMCA diag
// H's console runs, where readers waited with nothing ahead while 4.5 MB/s came in over 59
// peers. mpv's 64 MiB cache is then 16 s of video, and a window at one priority spread the swarm
// over a dozen pieces: the reader waited 31 to 44 s of the 128 s played with 16 MiB pieces (16
// to 31 s with 8 MiB), the player came within 0.5 to 4 s of a stall, and stalled when it started
// right after the first piece. It must not stall once it has started, its reader must wait under
// 20 s in all (here 7 to 17 s), it must keep 2 s cached whenever the reader waits (here 4 s or
// more; the first seconds after the start ride on the swarm's ramp), and the engine must not
// download much twice (no end-game for good). A generated 1 GiB file, a fresh engine and swarm
// for 8 and for 16 MiB pieces; about 5 minutes in all.
static void swarmChecks() {
    std::printf("swarm\n");
    const std::string dir = "/tmp/app-checks/swarm";
    const std::string name = "generated.bin";
    const int64_t MiB = 1 << 20;
    const int64_t bitrate = 4 * MiB;
    std::string mk = "mkdir -p " + dir;
    CHECK(std::system(mk.c_str()) == 0 && swarm::generateFile(dir + "/" + name, 1024 * MiB));
    // 40 peers sending 8237 KiB/s in all (twice the bitrate), from 309 down to 103 KiB/s; every
    // fourth one has every piece, the others about 40% of them, so the pieces' availability
    // differs, as in a swarm where most peers are downloaders themselves
    std::vector<swarm::Peer> peers;
    for (int i = 0; i < 40; ++i) peers.push_back({8256 * (117 - 2 * i) / 3120, i % 4 == 0 ? 1.0 : 0.4});
    int total = 0;
    for (const swarm::Peer& peer : peers) total += peer.uploadKiB;
    for (int pieceMiB : {8, 16}) {
        swarm::Torrent t = swarm::makeTorrent(dir, name, pieceMiB * int(MiB));
        std::printf("  %d MiB pieces, %zu peers sending %d KiB/s in all, read at %lld MiB/s\n", pieceMiB, peers.size(),
                    total, (long long)(bitrate / MiB));
        swarm::Seeders seeders(t, dir, peers, 17001);
        swarm::Tracker tracker(11486, seeders.endpoints);
        CHECK(tracker.ok);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string counts;
        for (int n : seeders.pieceCounts()) counts += " " + std::to_string(n);
        std::printf("  pieces each peer has (of %d):%s\n", t.info->num_pieces(), counts.c_str());
        torrent::EngineSettings s = engineSettings("/tmp/app-checks/swarm-engine");
        s.defaultTrackers.clear();
        Playback p;
        {
            torrent::TorrentEngine engine(s);
            torrent::StreamRequest req = request(t.infoHash, 0);
            req.trackers = {"http://127.0.0.1:11486/announce"};
            p = play(engine, req, 512 * MiB, bitrate, seeders);
        }
        const double redundant = double(p.downloaded - p.kept) / MiB;
        std::printf("  %d MiB pieces: start %.1f s, %d stalls for %.1f s in %.1f s played, %.1f s cached at the least; "
                    "%d reads waited %.1f s; downloaded %.1f MiB (%.2f MiB/s), kept %.1f MiB, %.1f MiB more than kept\n",
                    pieceMiB, p.startSecs, p.stalls, p.stalledSecs, p.playedSecs, p.leastAheadSecs, p.waits,
                    p.waitedSecs, double(p.downloaded) / MiB,
                    double(p.downloaded) / MiB / std::max(1.0, p.startSecs + p.playedSecs), double(p.kept) / MiB,
                    redundant);
        CHECK(p.ok);
        CHECK(p.stalls == 0);
        CHECK(p.waitedSecs < 20);
        CHECK(p.leastAheadSecs >= 2);
        CHECK(redundant < 0.05 * double(p.kept) / MiB);
    }
    std::string rm = "rm -rf " + dir;
    CHECK(std::system(rm.c_str()) == 0);
}

int main(int argc, char** argv) {
    bool online = false, swarmCheck = false;
    std::string fullDisk;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--online") online = true;
        else if (a == "--swarm") swarmCheck = true;
        else if (a == "--full-disk" && i + 1 < argc) fullDisk = argv[++i];
    }
    std::signal(SIGPIPE, SIG_IGN);
    serverChecks();
    engineChecks();
    stateChecks();
    serviceChecks();
    if (online) dhtOnlineChecks();
    if (online) onlineChecks();
    if (online && !fullDisk.empty()) fullDiskChecks(fullDisk);
    if (swarmCheck) swarmChecks();
    if (failures == 0) std::printf("app_checks: all passed\n");
    return failures ? 1 : 0;
}
