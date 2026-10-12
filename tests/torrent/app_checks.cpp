// Laptop checks for what the app needs from the torrent engine, the server and
// TorrentService. Offline checks use dead info hashes and need no torrent data;
// --online also buffers the start of Sintel (Blender Foundation, CC BY). Build and run, from
// the repo root, in the harness image (tests/torrent/Dockerfile):
//   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src gmca-torrent-harness
//     bash -c 'cmake -S tests/torrent -B build-torrent && cmake --build build-torrent -j$(nproc)'
//   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//     gmca-torrent-harness build-torrent/app_checks [--online]
// The checks listen on ports 11481 to 11484 of the container's own 127.0.0.1.

#include <torrent/stream_url.hpp>
#include <torrent/torrent_engine.hpp>
#include <torrent/torrent_server.hpp>
#include <torrent/torrent_service.hpp>
#include <torrent/ui_model.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
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
    CHECK(service.start());
    CHECK(service.availability() == torrent::Availability::Ready);

    // the Settings switch: off refuses streams and prefetches, on serves again
    service.setEnabled(false);
    CHECK(service.availability() == torrent::Availability::Off);
    CHECK(startsWith(httpRequest(11483, "GET", "/" + dead(6) + "/0"), "HTTP/1.1 503"));
    service.prefetch(request(dead(6)));
    service.setEnabled(true);
    CHECK(service.availability() == torrent::Availability::Ready);

    // prefetch returns at once: the worker makes the engine call
    Clock::time_point t0 = Clock::now();
    service.prefetch(request(dead(7)));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(20));
    service.watch(dead(7));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(service.snapshot().known);
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
    for (int i = 0; i < 120 && calls.count() == 0; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(calls.count() == 1 && calls.last() == torrent::Status::Ok);
    CHECK(w->target() == torrent::startBufferBytes && w->buffered() == torrent::startBufferBytes);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    torrent::Stats s = service.snapshot();
    CHECK(s.known && s.hasMetadata && s.peers > 0 && s.error == torrent::Status::Ok);
    // while the warm-up holds its reader, the server answers for the same file
    std::string head = httpRequest(11484, "HEAD", "/" + sintel + "/5");
    CHECK(startsWith(head, "HTTP/1.1 200") && head.find("Content-Length: 129241752") != std::string::npos);
    w->finish();
    service.stop();
}

int main(int argc, char** argv) {
    const bool online = argc > 1 && std::string(argv[1]) == "--online";
    std::signal(SIGPIPE, SIG_IGN);
    serverChecks();
    engineChecks();
    serviceChecks();
    if (online) onlineChecks();
    if (failures == 0) std::printf("app_checks: all passed\n");
    return failures ? 1 : 0;
}
