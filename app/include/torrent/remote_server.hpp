// A Stremio streaming server set in Settings (Torrents, "Streaming server"): torrent streams and
// downloads go through it instead of this device's engine, which then only starts when the
// server doesn't answer. Its API, as Stremio's own apps use it (checked against stremio/server
// 4.22): GET /settings answers when it is there; POST /<hash>/create with the stream's trackers
// registers the torrent and, when the stream names no file, guesses one; GET /<hash>/<file index>
// streams it. File index -1, the local server's "largest file", isn't one it takes (HTTP 500),
// and it ignores a URL's tr=, so the trackers go in the create call.
// The pure parts are here (tests/test_remote_server.cpp); the requests are in remote_server.cpp.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include <torrent/stream_url.hpp>

namespace torrent::remote {

/// The address as typed, made usable: trimmed, "http://" in front when it has no scheme, no
/// trailing slash. "" for an empty one, and for one that isn't http(s) or has no host.
inline std::string normalizeServerUrl(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return "";
    const size_t scheme = s.find("://");
    if (scheme == std::string::npos) {
        s = "http://" + s;
    } else {
        std::string name = s.substr(0, scheme);
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name != "http" && name != "https") return "";
        s = name + s.substr(scheme);
    }
    const size_t host = s.find("://") + 3;
    while (s.size() > host && s.back() == '/') s.pop_back();
    if (host >= s.size() || s[host] == ':' || s[host] == '/') return "";
    for (size_t i = host; i < s.size(); ++i)
        if (std::isspace(static_cast<unsigned char>(s[i]))) return "";
    return s;
}

/// The body of POST /<hash>/create: the trackers as "tracker:<url>" sources and DHT, as Stremio's
/// apps send them, and a file guess when the request names no file.
inline nlohmann::json createBody(const StreamRequest& r) {
    nlohmann::json sources = nlohmann::json::array();
    for (const auto& t : r.trackers) sources.push_back("tracker:" + t);
    sources.push_back("dht:" + r.infoHash);
    nlohmann::json body = {
        {"torrent", {{"infoHash", r.infoHash}}},
        {"peerSearch", {{"sources", sources}, {"min", 40}, {"max", 150}}},
    };
    if (r.fileIndex < 0) body["guessFileIdx"] = nlohmann::json::object();
    return body;
}

/// The file to stream, from what the server says of the torrent (`stats`, the create call's
/// answer or /<hash>/stats.json): the request's own file, else the server's guess, else the file
/// the request's name hint names, else the largest. -1 while the server has no file list yet
/// (the torrent's metadata hasn't come).
inline int chooseFile(const StreamRequest& r, const nlohmann::json& stats) {
    if (r.fileIndex >= 0) return r.fileIndex;
    auto guessed = stats.find("guessedFileIdx");
    if (guessed != stats.end() && guessed->is_number_integer() && guessed->get<int>() >= 0) return guessed->get<int>();
    auto files = stats.find("files");
    if (files == stats.end() || !files->is_array() || files->empty()) return -1;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto baseName = [](const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };
    for (const auto& hint : r.fileHints) {
        const std::string want = lower(baseName(hint));
        for (size_t i = 0; i < files->size(); ++i) {
            const auto& f = (*files)[i];
            if (f.is_object() && f.contains("name") && f["name"].is_string() &&
                lower(baseName(f["name"].get<std::string>())) == want)
                return int(i);
        }
    }
    int best = -1;
    double bestLength = -1;
    for (size_t i = 0; i < files->size(); ++i) {
        const auto& f = (*files)[i];
        if (!f.is_object() || !f.contains("length") || !f["length"].is_number()) continue;
        const double length = f["length"].get<double>();
        if (length > bestLength) {
            best = int(i);
            bestLength = length;
        }
    }
    return best;
}

/// Where the server streams one file of a torrent.
inline std::string streamUrl(const std::string& server, const std::string& infoHash, int fileIdx) {
    return server + "/" + infoHash + "/" + std::to_string(fileIdx);
}

// ---- remote_server.cpp ---------------------------------------------------------------------

/// The server set in Settings, normalized; "" when there's none. configure() takes the setting
/// as typed (at startup and when Settings changes it) and forgets the routes to the last one.
/// Any thread.
std::string configured();
void configure(const std::string& typed);

/// The server answers GET /settings within 2 s. Blocks: a pool thread.
bool reachable(const std::string& server);

/// Registers the stream on the server and returns where the server streams it, waiting up to
/// 30 s for the file list when the stream names no file; "" with `why` set when it can't.
/// Blocks: a pool thread.
std::string open(const std::string& server, const StreamRequest& r, std::string& why);

/// The torrent's peers and download speed (bytes per second) as the server says, for the
/// loading screen; false when it doesn't answer within 1 s. Blocks: a pool thread.
bool peersAndSpeed(const std::string& server, const std::string& infoHash, int& peers, int64_t& bytesPerSec);

/// The server URL a local stream URL plays from once open() gave one, "" otherwise. Any thread.
std::string route(const std::string& localUrl);
void setRoute(const std::string& localUrl, const std::string& serverUrl);

/// Starts this device's engine if a streaming server kept it from starting. The UI thread
/// only, like TorrentService::start().
void startLocalEngine();

/// Where a download of a local stream URL fetches from: the server when it answers (the
/// stream opened there), else the local URL, with this device's engine started and a
/// notification. Throws when the server answers but can't open the stream. Blocks: a pool
/// thread.
std::string downloadSource(const std::string& localUrl);

}  // namespace torrent::remote
