// What the app shows about torrents: the messages for each failure, and the loading
// screen's stages, progress and pulse. Header-only, free of borealis: it hands back i18n
// keys, the views look them up (tests/test_torrent_ui.cpp).
#pragma once

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

}  // namespace torrent
