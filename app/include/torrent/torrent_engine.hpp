// Torrent engine: one libtorrent session, started on demand, that downloads the
// parts of a torrent readers ask for and deletes everything once they're done.
// libtorrent stays out of this header so the rest of GMCA doesn't need its headers
// (or, on PS4, the compat flags they're built with).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <torrent/stream_url.hpp>

namespace torrent {

enum class Status { Ok, BadRequest, NotFound, NoPeers, DiskError, Stopped };

inline const char* statusName(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::BadRequest: return "bad request";
        case Status::NotFound: return "file not found";
        case Status::NoPeers: return "no peers";
        case Status::DiskError: return "disk error";
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
    int peers = 0;
    int downloadBytesPerSec = 0;
    bool hasMetadata = false;
};

struct EngineSettings {
    std::string tempDir;        // owned by the engine: emptied at start and on exit
    int uploadLimit = 1 << 20;  // bytes per second, only while torrents are open
    int graceSecs = 30;         // keep a torrent this long after its last reader
    int stallSecs = 60;         // no metadata, or no new data, for this long: NoPeers
    int idleSecs = 300;         // close the session after this long with no torrents
    std::function<void(const std::string&)> log;
};

/// One open reader of one file in a torrent (for the server: one HTTP connection).
using ReaderId = uint64_t;

class TorrentEngine {
public:
    explicit TorrentEngine(EngineSettings settings);
    ~TorrentEngine();
    TorrentEngine(const TorrentEngine&) = delete;
    TorrentEngine& operator=(const TorrentEngine&) = delete;

    /// Adds the torrent if needed, waits for its metadata and picks the file. On Ok
    /// the caller holds `reader` until release(req, reader).
    Status acquire(const StreamRequest& req, FileInfo& file, ReaderId& reader);
    void release(const StreamRequest& req, ReaderId reader);

    /// Blocks until bytes [begin, end) of the reader's file are downloaded and
    /// verified, steering that reader's download window to `begin`.
    Status waitFor(const StreamRequest& req, ReaderId reader, int64_t begin, int64_t end);

    /// Starts fetching metadata and peers without downloading pieces; dropped after
    /// two minutes unless a reader acquires it.
    void prefetch(const StreamRequest& req);

    Stats stats(const std::string& infoHash);
    void setUploadLimit(int bytesPerSec);

    /// Wakes every waiter with Stopped and refuses new work. Call before tearing
    /// down anything that still calls into the engine.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace torrent
