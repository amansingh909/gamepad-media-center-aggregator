// What the app shows about torrents: the messages for each failure, and the loading
// screen's stages, progress and pulse. Header-only, free of borealis: it hands back i18n
// keys, the views look them up (tests/test_torrent_ui.cpp). Also how the player lets go of
// a warm-up (tests/torrent/app_checks.cpp, which links the service).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <torrent/torrent_service.hpp>

namespace torrent {

/// Why torrents can't start right now, as an i18n key, for the grey rows and the player.
inline const char* availabilityKey(Availability a) {
    switch (a) {
        case Availability::Off: return "main/torrent/error/off";
        case Availability::NotStarted: return "main/torrent/error/start";
        case Availability::Ready:
        case Availability::Stopped: break;
    }
    return "main/torrent/error/unplayable";
}

/// Why a torrent didn't load, as an i18n key; "" for Ok and Stopped (a cancel says nothing).
inline const char* errorKey(Status s) {
    switch (s) {
        case Status::NoPeers: return "main/torrent/error/no_peers";
        case Status::DiskError: return "main/torrent/error/no_space";
        case Status::BadRequest:
        case Status::NotFound:
        case Status::Failed: return "main/torrent/error/unplayable";
        case Status::Ok:
        case Status::Stopped: break;
    }
    return "";
}

/// mpv failed on a torrent: the engine's own error if it has one, else the server's answer
/// as mpv reports it ("http: HTTP error 504 Gateway Timeout"), else "can't be played".
inline const char* playerErrorKey(const std::string& mpvError, Status engineError) {
    if (engineError == Status::DiskError || engineError == Status::Failed) return errorKey(engineError);
    if (mpvError.find("HTTP error 504") != std::string::npos) return errorKey(Status::NoPeers);
    if (mpvError.find("HTTP error 507") != std::string::npos) return errorKey(Status::DiskError);
    if (mpvError.find("HTTP error 503") != std::string::npos) return availabilityKey(Availability::Off);
    return errorKey(Status::Failed);
}

enum class LoadingStage { FindingPeers, GettingInfo, Buffering };

struct LoadingState {
    LoadingStage stage = LoadingStage::FindingPeers;
    int percent = 0;        // of the start buffer, for "Buffering 45%"
    float progress = 0.f;   // the bar, 0 to 1
    int peers = 0;
    int bytesPerSec = 0;
};

/// The loading screen from the engine's stats and the warm-up's progress: no metadata and
/// no peer yet is "Finding peers", peers but no metadata "Getting torrent info", then
/// "Buffering N%" of the `target` bytes. The bar holds 5% and 10% in the first two stages,
/// so it moves before data does, and fills the rest with the buffer.
inline LoadingState loadingState(const Stats& s, int64_t buffered, int64_t target) {
    LoadingState out;
    out.peers = s.peers;
    out.bytesPerSec = s.downloadBytesPerSec;
    if (!s.hasMetadata && target <= 0) {
        out.stage = s.peers > 0 ? LoadingStage::GettingInfo : LoadingStage::FindingPeers;
        out.progress = s.peers > 0 ? 0.10f : 0.05f;
        return out;
    }
    out.stage = LoadingStage::Buffering;
    double f = target > 0 ? double(buffered) / double(target) : 0.0;
    f = std::min(1.0, std::max(0.0, f));
    out.percent = int(f * 100);
    out.progress = float(0.10 + 0.90 * f);
    return out;
}

/// "3.4 MB/s" from 1 MiB/s up, "512 KB/s" below.
inline std::string formatRate(int bytesPerSec) {
    char text[32];
    if (bytesPerSec >= (1 << 20))
        std::snprintf(text, sizeof text, "%.1f MB/s", double(bytesPerSec) / (1 << 20));
    else
        std::snprintf(text, sizeof text, "%d KB/s", std::max(0, bytesPerSec) / 1024);
    return text;
}

/// The logo's slow pulse: alpha from 1 down to 0.45 and back, every 2.4 seconds.
inline float pulseAlpha(double seconds) {
    const double period = 2.4, pi = 3.14159265358979323846;
    double phase = std::fmod(std::max(0.0, seconds), period) / period;
    return float(0.725 + 0.275 * std::cos(2 * pi * phase));
}

/// The player is done with a warm-up. Once its start is in (`ready`, `done` said Ok) the
/// reader just goes; before that the warm-up is cancelled, which also ends its wait for
/// metadata or data at once (finish() alone would leave it downloading, and uploading,
/// until its bytes are in or the stall timeout).
inline void endWarmup(Warmup& w, bool ready) {
    if (ready)
        w.finish();
    else
        w.cancel();
}

}  // namespace torrent
