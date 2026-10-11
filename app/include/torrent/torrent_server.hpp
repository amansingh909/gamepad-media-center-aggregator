// A small HTTP server in front of TorrentEngine, using Stremio's streaming server
// URLs, so mpv and the downloader read torrents like any other URL.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace torrent {

class TorrentEngine;

class TorrentServer {
public:
    TorrentServer(TorrentEngine& engine, std::function<void(const std::string&)> log = {});
    ~TorrentServer();
    TorrentServer(const TorrentServer&) = delete;
    TorrentServer& operator=(const TorrentServer&) = delete;

    /// Listens on 127.0.0.1, or on every interface when `lan` is set (testing from
    /// another machine). Returns false with `error` set if the port is taken.
    bool start(uint16_t port, bool lan, std::string& error);

    /// Stops accepting, shuts the engine down, unblocks every open connection and
    /// waits for them to finish.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace torrent
