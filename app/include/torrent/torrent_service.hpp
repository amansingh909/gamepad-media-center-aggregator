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

/// The watched torrent's stats as snapshot() reports them, from a fresh engine reading and the
/// last report: the engine's word while it has the torrent, and once it has dropped it (30 s
/// after its last reader) the error it last reported. A stream the engine broke off (a full
/// disk) reaches the player as an early end of the file only after mpv has played its cache,
/// which can be later than that.
inline Stats keepLastError(Stats fresh, const Stats& last) {
    if (!fresh.known) fresh.error = last.error;
    return fresh;
}

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
    std::string stateFile;      // absolute; the engine's DHT state between runs ({config}/torrent-dht.dat)
    bool portMapping = false;   // Settings, Torrents, "Allow incoming connections (UPnP)"
    /// Called from several threads at once, sometimes with the engine's lock held: it must
    /// not block on another thread or call into the service or the engine.
    std::function<void(const std::string&)> log;
};

/// The start of one file, buffered for the player (TorrentService::warmUp()). While the
/// player keeps it, the engine keeps its reader, so that stretch and the file's tail stay
/// wanted until mpv has data.
/// Dropping the last handle without finish() or cancel() ends the warm-up the same way as
/// cancel(), so a handle that is forgotten never leaves a reader behind.
class Warmup {
public:
    struct State;
    explicit Warmup(std::shared_ptr<State> state);
    ~Warmup();  // dropping the handle ends the warm-up, like cancel()
    Warmup(const Warmup&) = delete;
    Warmup& operator=(const Warmup&) = delete;

    /// Bytes from the start of the file confirmed so far, and how many it waits for (0
    /// until the torrent's metadata is in). Any thread.
    int64_t buffered() const;
    int64_t target() const;
    /// Bytes of [0, target()) in so far for the loading screen: also counts the written
    /// blocks of pieces still downloading (the service's worker refreshes it twice a
    /// second), so it moves before a whole piece is verified. At least buffered(), and it
    /// never goes down. Any thread.
    int64_t progress() const;
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

    /// Starts the server, and on the worker the engine's session, which stays open while
    /// streaming is on (setEnabled()). False if it couldn't listen: torrent rows then stay
    /// grey, and startError() says why. Call once, from the thread that calls stop().
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
    /// UPnP and NAT-PMP on or off, applied by the worker. Any thread.
    void setPortMapping(bool on);

    /// Fetches the torrent's metadata and peers in the background; returns at once.
    void prefetch(const StreamRequest& req);

    /// Keeps a Stats for one torrent ("" for none), refreshed every half second by the
    /// worker; snapshot() returns the latest without waiting. Its error outlives the
    /// engine's copy of the torrent (keepLastError()) until another watch() changes the hash.
    void watch(const std::string& infoHash);
    Stats snapshot() const;
    /// The download speed the watched torrents have had while they moved data, in bytes per
    /// second (a slow average, buffer_hint.hpp); 0 until one did in this app session. Only
    /// samples taken while a reader has the torrent open count. Any thread.
    int64_t recentDownloadRate() const;

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
