// Laptop harness for the torrent engine and server.
//   torrent_serve --fetch /<hash>/<index>   engine only: fetch the first MiB, then exit
//   torrent_serve [options]                 serve http://127.0.0.1:11470 (Task 4)
// Options: --temp DIR --port N --grace S --stall S --idle S --upload BYTES --lan
//   --pause S   fetch mode: after the first MiB, keep the reader idle for S seconds,
//               then read a MiB from the middle of the file

#include <torrent/stream_url.hpp>
#include <torrent/torrent_engine.hpp>
#include <torrent/torrent_server.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

static volatile std::sig_atomic_t stopRequested = 0;
static void onSignal(int) { stopRequested = 1; }

static void logLine(const std::string& msg) {
    std::time_t t = std::time(nullptr);
    char ts[16];
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
    std::printf("[%s] %s\n", ts, msg.c_str());
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    torrent::EngineSettings settings;
    settings.tempDir = "/tmp/torrent-serve";
    settings.log = logLine;
    int port = 11470;
    bool lan = false;
    std::string fetch;
    int pauseSecs = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--temp") settings.tempDir = value();
        else if (a == "--port") port = std::atoi(value().c_str());
        else if (a == "--grace") settings.graceSecs = std::atoi(value().c_str());
        else if (a == "--stall") settings.stallSecs = std::atoi(value().c_str());
        else if (a == "--idle") settings.idleSecs = std::atoi(value().c_str());
        else if (a == "--upload") settings.uploadLimit = std::atoi(value().c_str());
        else if (a == "--lan") lan = true;
        else if (a == "--fetch") fetch = value();
        else if (a == "--pause") pauseSecs = std::atoi(value().c_str());
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    torrent::TorrentEngine engine(settings);

    if (!fetch.empty()) {
        torrent::StreamRequest req;
        if (!torrent::parseStreamPath(fetch, req)) {
            logLine("bad path: " + fetch);
            return 2;
        }
        torrent::FileInfo file;
        torrent::ReaderId reader = 0;
        torrent::Status st = engine.acquire(req, file, reader);
        if (st != torrent::Status::Ok) {
            logLine(std::string("acquire: ") + torrent::statusName(st));
            return 1;
        }
        logLine("file " + file.name + ", " + std::to_string(file.size) + " bytes, at " + file.diskPath);
        st = engine.waitFor(req, reader, 0, std::min<int64_t>(file.size, 1 << 20));
        logLine(std::string("first MiB: ") + torrent::statusName(st));
        if (st == torrent::Status::Ok && pauseSecs >= 0) {
            // same reader: a paused player that keeps its connection
            logLine("pausing " + std::to_string(pauseSecs) + " s");
            std::this_thread::sleep_for(std::chrono::seconds(pauseSecs));
            int64_t begin = file.size / 2;
            st = engine.waitFor(req, reader, begin, std::min<int64_t>(file.size, begin + (1 << 20)));
            logLine(std::string("after the pause: ") + torrent::statusName(st));
        }
        engine.release(req, reader);
        return st == torrent::Status::Ok ? 0 : 1;
    }

    torrent::TorrentServer server(engine, logLine);
    std::string error;
    if (!server.start(uint16_t(port), lan, error)) {
        logLine("server: " + error);
        return 1;
    }
    while (!stopRequested) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    logLine("stopping");
    server.stop();
    return 0;
}
