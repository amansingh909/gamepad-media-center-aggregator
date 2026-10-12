// Torrent engine: one libtorrent session, started on demand, that downloads the
// parts of a torrent readers ask for and deletes everything once they're done.
// libtorrent stays out of this header so the rest of GMCA doesn't need its headers
// (or, on PS4, the compat flags they're built with).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <torrent/stream_url.hpp>

namespace torrent {

enum class Status { Ok, BadRequest, NotFound, NoPeers, DiskError, Failed, Stopped };

inline const char* statusName(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::BadRequest: return "bad request";
        case Status::NotFound: return "file not found";
        case Status::NoPeers: return "no peers";
        case Status::DiskError: return "disk full";
        case Status::Failed: return "torrent error";
        case Status::Stopped: return "stopped";
    }
    return "unknown";
}

struct FileInfo {
    std::string name;      // path inside the torrent
    std::string diskPath;  // where the engine writes it
    int64_t size = 0;
};

struct Stats {
    bool known = false;  // the engine has this torrent right now
    bool hasMetadata = false;
    int peers = 0;
    int downloadBytesPerSec = 0;
    Status error = Status::Ok;  // DiskError (no space left) or Failed (any other file or torrent error)
};

struct EngineSettings {
    std::string tempDir;        // absolute; the engine makes <info hash>-<n> folders in it and deletes only those
    int uploadLimit = 1 << 20;  // bytes per second, only while torrents are open
    int graceSecs = 30;         // keep a torrent this long after its last reader
    int stallSecs = 60;         // no metadata, or no new data, for this long: NoPeers
    int idleSecs = 300;         // close the session after this long with no torrents, unless kept open
    /// Called from the engine's thread and its callers' threads, at the same time, and with
    /// the engine's lock held: it must not block on another thread or call into the engine.
    std::function<void(const std::string&)> log;
    /// Where the session's DHT state is kept between runs (absolute; "" keeps none). Outside
    /// tempDir: the sweep empties that folder.
    std::string stateFile;
    /// Added to every torrent: Stremio addons often send a bare info hash (Torrentio sends no
    /// trackers at all), and the DHT alone found no peer in 90 s for a seeded torrent that a
    /// few trackers served at once. Well-known public trackers, UDP or plain HTTP only (the
    /// PS4 build has no TLS); one that is down just doesn't answer.
    std::vector<std::string> defaultTrackers = {
        "udp://tracker.opentrackr.org:1337/announce",
        "udp://open.stealth.si:80/announce",
        "udp://explodie.org:6969/announce",
        "udp://tracker.torrent.eu.org:451/announce",
        "udp://open.demonii.com:1337/announce",
        "udp://exodus.desync.com:6969/announce",
        "udp://tracker.tiny-vps.com:6969/announce",
        "udp://tracker.dler.org:6969/announce",
        "udp://opentracker.io:6969/announce",
        "udp://tracker.theoks.net:6969/announce",
        "udp://tracker-udp.gbitt.info:80/announce",
        "udp://tracker.qu.ax:6969/announce",
        "udp://tracker.filemail.com:6969/announce",
        "http://tracker.opentrackr.org:1337/announce",
        "http://tracker.bt4g.com:2095/announce",
    };
};

/// One open reader of one file in a torrent (for the server: one HTTP connection).
using ReaderId = uint64_t;

/// Every request's infoHash must be 40 lowercase hex characters (parseStreamPath() and
/// parseLocalStreamUrl() make them so); the engine refuses any other. prefetch(), stats(),
/// bytesReady() and setUploadLimit() take the engine's lock and ask libtorrent's thread for
/// answers: the app calls them from TorrentService's worker, never from the UI thread.
class TorrentEngine {
public:
    explicit TorrentEngine(EngineSettings settings);
    ~TorrentEngine();
    TorrentEngine(const TorrentEngine&) = delete;
    TorrentEngine& operator=(const TorrentEngine&) = delete;

    /// Adds the torrent if needed, waits for its metadata and picks the file. On Ok
    /// the caller holds `reader` until release(req, reader).
    ///
    /// `abandoned`, if set, is asked at least once a second while the call waits; when
    /// it returns true (the caller's client went away), the call gives up with Stopped.
    /// It runs with the engine's lock held: it must not block, and must not call into
    /// the engine.
    Status acquire(const StreamRequest& req, FileInfo& file, ReaderId& reader,
                   const std::function<bool()>& abandoned = {});
    void release(const StreamRequest& req, ReaderId reader);

    /// Blocks until bytes [begin, end) of the reader's file are downloaded and
    /// verified, steering that reader's download window to `begin`. Keep `end - begin`
    /// small (the server asks for 256 KiB): a range longer than half the reader's window
    /// never completes. `abandoned` as for acquire(): asked at least once a second while
    /// the data isn't there yet, run with the engine's lock held, so it must not block or
    /// call into the engine.
    Status waitFor(const StreamRequest& req, ReaderId reader, int64_t begin, int64_t end,
                   const std::function<bool()>& abandoned = {});

    /// Starts fetching metadata and peers without downloading pieces, on few
    /// connections; dropped after two minutes unless a reader acquires it, or at once
    /// when more than three prefetched torrents are waiting for a reader (the oldest).
    /// A torrent the engine already has with no reader (its grace period, or an earlier
    /// prefetch) is kept two minutes from now instead.
    void prefetch(const StreamRequest& req);

    Stats stats(const std::string& infoHash);
    /// Bytes of [begin, end) of the reader's file that are in: verified pieces whole, and
    /// the written blocks of the pieces still downloading (what a progress bar can show
    /// before waitFor() returns). -1 when the reader is gone or the engine has stopped.
    int64_t bytesReady(const StreamRequest& req, ReaderId reader, int64_t begin, int64_t end);
    void setUploadLimit(int bytesPerSec);

    /// On: the session starts now if it hasn't, and stays open with no torrents, so the DHT is
    /// warm for the next one. Off: it closes after idleSecs with no torrents, as before. Blocks
    /// while a session starts: the app calls it from TorrentService's worker.
    void keepSessionOpen(bool on);

    /// Wakes every waiter with Stopped and refuses new work, for good. Call before
    /// tearing down anything that still calls into the engine.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace torrent
