// Torrent streaming as the app uses it: the engine, the local server, and a worker
// thread for the engine calls the UI must not wait on. One per app, made by main().
// No libtorrent or borealis here: the laptop harness builds it too.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <torrent/stream_url.hpp>
#include <torrent/torrent_engine.hpp>

namespace torrent {

/// Bytes at the start of a file buffered before the player opens it.
constexpr int64_t startBufferBytes = 4ll << 20;

/// Whether torrent rows can play right now.
enum class Availability {
    Ready,       // they play
    Off,         // the Settings switch is off
    NotStarted,  // the server couldn't listen (port taken): startError() says why
    Stopped,     // the app is closing
};

struct ServiceSettings {
    std::string tempDir;        // absolute, the engine's own folder ({config}/torrents)
    uint16_t port = 11470;      // Stremio's streaming server port
    bool lan = false;           // listen on every interface (testing builds only)
    bool enabled = true;        // the Settings switch
    int uploadLimit = 1 << 20;  // bytes per second, while someone reads
    /// Called from several threads at once, sometimes with the engine's lock held: it must
    /// not block on another thread or call into the service or the engine.
    std::function<void(const std::string&)> log;
};

/// The start of one file, buffered for the player (TorrentService::warmUp()). While the
/// player keeps it, the engine keeps its reader, so that stretch and the file's tail stay
/// wanted until mpv has data.
class Warmup {
public:
    struct State;
    explicit Warmup(std::shared_ptr<State> state);

    /// Bytes from the start of the file confirmed so far, and how many it waits for (0
    /// until the torrent's metadata is in). Any thread.
    int64_t buffered() const;
    int64_t target() const;
    /// The player has its data: the reader goes. Any thread, any number of times.
    void finish();
    /// Gives up: `done` runs with Stopped within about a second if it hasn't run yet, and
    /// the reader goes. Any thread, any number of times.
    void cancel();

private:
    std::shared_ptr<State> state;
};

class TorrentService {
public:
    explicit TorrentService(ServiceSettings settings);
    ~TorrentService();  // stop(), then the engine deletes its folders
    TorrentService(const TorrentService&) = delete;
    TorrentService& operator=(const TorrentService&) = delete;

    /// Starts the server. False if it couldn't listen: torrent rows then stay grey, and
    /// startError() says why. Call once, from the thread that calls stop().
    bool start();
    std::string startError() const;

    /// For good (app exit): every warm-up gives up, the server and the engine stop, every
    /// thread is joined.
    void stop();

    Availability availability() const;

    /// The Settings switch. Off refuses new streams, downloads and prefetches (the server
    /// answers 503) and lets the open ones finish; stop() can't be undone, so this is how
    /// torrents go off. Any thread.
    void setEnabled(bool on);
    /// Applied by the worker. Any thread.
    void setUploadLimit(int bytesPerSec);

    /// Fetches the torrent's metadata and peers in the background; returns at once.
    void prefetch(const StreamRequest& req);

    /// Keeps a Stats for one torrent ("" for none), refreshed every half second by the
    /// worker; snapshot() returns the latest without waiting.
    void watch(const std::string& infoHash);
    Stats snapshot() const;

    /// Buffers bytes [0, min(bytes, file size)) of the request's file on a thread of its
    /// own. `done` runs once, on that thread: Ok once the bytes are in (the reader stays
    /// until finish() or cancel()), or why not (Stopped after cancel() or stop()). When
    /// torrents aren't Ready, it runs at once on the caller's thread with Stopped.
    std::shared_ptr<Warmup> warmUp(const StreamRequest& req, int64_t bytes, std::function<void(Status)> done);

    /// The app's service: nullptr before main() makes it and once the app is closing.
    static TorrentService* current();
    static void setCurrent(TorrentService* service);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace torrent
