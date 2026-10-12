// Stremio torrent streams as GMCA source rows: the local server's URL for a stream, the
// request back from that URL, and what the rows show and sort by. Header-only and free of
// libtorrent and borealis (tests/test_torrent_source.cpp).
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <torrent/stream_url.hpp>

namespace torrent {

/// Where TorrentService's server listens (ServiceSettings::port).
constexpr const char* localStreamBase = "http://127.0.0.1:11470";

/// mpv's network-timeout for a local stream URL, in seconds. The engine waits up to its
/// stall timeout (60 s) before it answers or gives up on a stalled stream, and the loading
/// screen reports that; mpv must not give up first.
constexpr int playerNetworkTimeoutSecs = 120;

/// mpv's options for a local stream URL: the timeout above, and no reconnect. mpv turns
/// ffmpeg's reconnect on, and ffmpeg then retries a body that ended early four times; against
/// a stalled torrent each retry waited the engine's whole stall timeout before its 504, so
/// "No peers" came five minutes after the data stopped, not one. The server ends a body early
/// only when the engine gives up on the torrent or the app stops, where a retry can't help.
/// Seeks open their own connections and don't depend on it.
inline std::string playerOptions() {
    return "network-timeout=" + std::to_string(playerNetworkTimeoutSecs) + ",stream-lavf-o=reconnect=0";
}

/// More trackers than this in one stream is an addon bug, not more peers.
constexpr size_t maxTrackers = 16;

/// A tracker URL or file name hint longer than this (raw bytes) is left out, so a local URL
/// stays far below the server's 16 KiB request head however much an addon sends.
constexpr size_t maxTrackerLength = 200;
constexpr size_t maxFilenameLength = 200;

/// The largest file index the server's URL parser accepts (6 digits).
constexpr int maxFileIndex = 999999;

/// The tracker URLs among Stremio's `sources` ("tracker:<url>", "dht:<info hash>"), without
/// duplicates and without over-long ones, at most maxTrackers.
inline std::vector<std::string> trackersFromSources(const std::vector<std::string>& sources) {
    const std::string prefix = "tracker:";
    std::vector<std::string> out;
    for (const auto& s : sources) {
        if (s.compare(0, prefix.size(), prefix) != 0) continue;
        std::string url = s.substr(prefix.size());
        if (url.empty() || url.size() > maxTrackerLength || std::find(out.begin(), out.end(), url) != out.end()) continue;
        out.push_back(url);
        if (out.size() == maxTrackers) break;
    }
    return out;
}

/// The request inside a local stream URL; false for any other URL.
inline bool parseLocalStreamUrl(const std::string& url, StreamRequest& out) {
    const std::string base = localStreamBase;
    if (url.compare(0, base.size(), base) != 0) return false;
    return parseStreamPath(url.substr(base.size()), out);
}

/// The local server's URL for a Stremio torrent stream, or "" when `infoHash` isn't 40 hex
/// characters or `fileIdx` is past what the server accepts, or when the URL wouldn't parse
/// back (the row then stays grey). An over-long file name only loses its hint. The hash goes in lowercase (the engine names its folders by it), the file's
/// name as the f= hint.
inline std::string localStreamUrl(const std::string& infoHash, int fileIdx, const std::vector<std::string>& sources,
                                  const std::string& filename) {
    StreamRequest r;
    r.infoHash = infoHash;
    for (char& c : r.infoHash) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!isInfoHash(r.infoHash)) return "";
    if (fileIdx > maxFileIndex) return "";
    r.fileIndex = fileIdx < 0 ? -1 : fileIdx;
    r.trackers = trackersFromSources(sources);
    if (!filename.empty() && filename.size() <= maxFilenameLength) r.fileHints.push_back(filename);
    std::string url = std::string(localStreamBase) + streamPath(r);
    StreamRequest back;
    if (!parseLocalStreamUrl(url, back)) return "";
    return url;
}

/// The extension the downloader gives a local stream's file: its name hint's ("mkv" for
/// f=Movie.mkv), lowercase, or "" when there's no usable one (the URL's path has none).
inline std::string localStreamExtension(const std::string& url) {
    StreamRequest r;
    if (!parseLocalStreamUrl(url, r) || r.fileHints.empty()) return "";
    const std::string& name = r.fileHints.front();
    const size_t dot = name.find_last_of('.');
    const size_t slash = name.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = name.substr(dot + 1);
    if (ext.empty() || ext.size() > 5) return "";
    for (char& c : ext) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return "";
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

/// The seeders an addon prints in a stream's text: Torrentio's "👤 45", "Seeders: 45" or
/// "45 seeders". -1 when there are none (unknown, never shown as 0). After PR #59's
/// parseSeeders.
inline int parseSeeders(const std::string& text) {
    auto number = [](const std::string& t, size_t i, int& out) {
        if (i >= t.size() || !std::isdigit(static_cast<unsigned char>(t[i]))) return false;
        long n = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) && n < 100000000)
            n = n * 10 + (t[i++] - '0');
        out = int(n);
        return true;
    };
    // 👤 (U+1F464) or 👥 (U+1F465), then spaces or a colon, then the number
    for (const char* emoji : {"\xF0\x9F\x91\xA4", "\xF0\x9F\x91\xA5"}) {
        size_t p = text.find(emoji);
        if (p == std::string::npos) continue;
        size_t i = p + 4;
        while (i < text.size() && (text[i] == ' ' || text[i] == ':')) ++i;
        int n = 0;
        if (number(text, i, n)) return n;
    }
    // "seed", "seeds", "seeder", "seeders", any case: the number after it, or before it
    std::string low = text;
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    size_t p = low.find("seed");
    if (p == std::string::npos) return -1;
    size_t i = p + 4;
    while (i < low.size() && std::isalpha(static_cast<unsigned char>(low[i]))) ++i;
    while (i < low.size() && (low[i] == ' ' || low[i] == ':')) ++i;
    int n = 0;
    if (number(low, i, n)) return n;
    size_t end = p;
    while (end > 0 && low[end - 1] == ' ') --end;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(low[begin - 1]))) --begin;
    if (begin < end && number(low, begin, n)) return n;
    return -1;
}

}  // namespace torrent
