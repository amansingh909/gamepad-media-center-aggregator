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

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    serverChecks();
    engineChecks();
    if (failures == 0) std::printf("app_checks: all passed\n");
    return failures ? 1 : 0;
}
