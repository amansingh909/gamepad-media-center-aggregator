// A small HTTP server in front of TorrentEngine, using Stremio's streaming server
// URLs, so mpv and the downloader read torrents like any other URL.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace torrent {

class TorrentEngine;

/// The engine must outlive the server. start() and stop() are called from one thread.
class TorrentServer {
public:
    /// `log` is called from every connection's thread, at the same time, and from inside
    /// the engine's lock: it must not block on another thread or call into the engine.
    TorrentServer(TorrentEngine& engine, std::function<void(const std::string&)> log = {});
    ~TorrentServer();  // stop()
    TorrentServer(const TorrentServer&) = delete;
    TorrentServer& operator=(const TorrentServer&) = delete;

    /// Listens on 127.0.0.1, or on every interface when `lan` is set (testing from
    /// another machine). A server starts once: false, with `error` set, if the port is
    /// taken, if it is already running, or after stop().
    bool start(uint16_t port, bool lan, std::string& error);

    /// Off: new requests get 503 "Torrent streaming is off", open ones carry on. This is
    /// the Settings switch: stop() can't be undone. Any thread.
    void setEnabled(bool on);

    /// Stops accepting, shuts the engine down (for good), unblocks every open connection
    /// and waits for them to finish. After a failed start(), or a second time, it only
    /// makes later start() calls fail.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace torrent
