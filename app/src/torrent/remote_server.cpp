// A Stremio streaming server set in Settings (torrent/remote_server.hpp): the requests to it, the
// routes from local stream URLs to it, and this device's engine when it doesn't answer.
#include "torrent/remote_server.hpp"

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <borealis.hpp>

#include "api/http.hpp"
#include "torrent/stremio_source.hpp"
#include "torrent/torrent_service.hpp"

using namespace brls::literals;

namespace torrent::remote {

namespace {
std::mutex mutex;
std::string server;                         // configure()'s, normalized
std::map<std::string, std::string> routes;  // local stream URL -> where the server streams it
}  // namespace

std::string configured() {
    std::lock_guard<std::mutex> lock(mutex);
    return server;
}

void configure(const std::string& typed) {
    std::lock_guard<std::mutex> lock(mutex);
    server = normalizeServerUrl(typed);
    routes.clear();
}

bool reachable(const std::string& url) {
    try {
        return nlohmann::json::parse(HTTP::get(url + "/settings", HTTP::Timeout{2000})).is_object();
    } catch (const std::exception& e) {
        brls::Logger::warning("streaming server {}: {}", url, e.what());
        return false;
    }
}

std::string open(const std::string& url, const StreamRequest& r, std::string& why) {
    nlohmann::json stats;
    try {
        stats = nlohmann::json::parse(HTTP::post(url + "/" + r.infoHash + "/create", createBody(r).dump(),
            HTTP::Header{"Content-Type: application/json"}, HTTP::Timeout{15000}));
    } catch (const std::exception& e) {
        why = e.what();
        return "";
    }
    int file = chooseFile(r, stats);
    // the file list comes with the torrent's metadata: until then, ask again
    for (int i = 0; file < 0 && i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try {
            stats = nlohmann::json::parse(HTTP::get(url + "/" + r.infoHash + "/stats.json", HTTP::Timeout{5000}));
        } catch (const std::exception&) {
            continue;
        }
        file = chooseFile(r, stats);
    }
    if (file < 0) {
        why = "no file list after 30 s";
        return "";
    }
    brls::Logger::info("streaming server {}: {} file {}", url, r.infoHash, file);
    return streamUrl(url, r.infoHash, file);
}

std::string route(const std::string& localUrl) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = routes.find(localUrl);
    return it == routes.end() ? "" : it->second;
}

void setRoute(const std::string& localUrl, const std::string& serverUrl) {
    std::lock_guard<std::mutex> lock(mutex);
    routes[localUrl] = serverUrl;
}

void startLocalEngine() {
    auto* service = TorrentService::current();
    if (!service || service->availability() != Availability::NotStarted) return;
    brls::Logger::info("torrent: the streaming server didn't answer, this device's engine starts");
    if (!service->start()) brls::Logger::error("torrent: {}", service->startError());
}

std::string downloadSource(const std::string& localUrl) {
    StreamRequest r;
    const std::string url = configured();
    if (url.empty() || !parseLocalStreamUrl(localUrl, r)) return localUrl;
    if (std::string via = route(localUrl); !via.empty()) return via;
    if (reachable(url)) {
        std::string why;
        std::string via = open(url, r, why);
        if (via.empty()) throw std::runtime_error("the streaming server couldn't start this torrent (" + why + ")");
        setRoute(localUrl, via);
        return via;
    }
    // this device's engine starts on the UI thread (TorrentService::start()); a bounded wait,
    // as the app may be closing and the UI thread no longer running its queue
    auto started = std::make_shared<std::promise<void>>();
    std::future<void> done = started->get_future();
    brls::sync([started]() {
        startLocalEngine();
        brls::Application::notify("main/torrent/server/fallback_download"_i18n);
        started->set_value();
    });
    done.wait_for(std::chrono::seconds(10));
    return localUrl;
}

}  // namespace torrent::remote
