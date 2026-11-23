// What the engine tells libtorrent about the PS4's network. libtorrent can't list the
// console's interfaces or routes (the TORRENT_PS4 patch), so the engine reads NetCtl before a
// session starts and every 30 s while one is open (it stays open for the app's life), and
// hands libtorrent each new reading. Header-only (tests/test_torrent_source.cpp); the engine
// uses it on PS4 only.
#pragma once

#include <string>

namespace torrent {

/// One reading of NetCtl: the console's IPv4 address, netmask and default route.
struct NetworkReading {
    bool ok = false;  // NetCtl answered, with addresses that parse
    std::string ip, netmask, gateway;
};

class NetworkWatch {
public:
    enum class Action {
        None,     // nothing new: libtorrent has these addresses, or the failure was logged already
        Changed,  // give libtorrent the new addresses, and reopen a running session's sockets
        Failed,   // the first failed reading in a row: log why, once
    };

    /// What to do with a new reading.
    Action update(const NetworkReading& r) {
        if (!r.ok) {
            if (failing) return Action::None;
            failing = true;
            return Action::Failed;
        }
        // back after a failure, even the same addresses count: the sockets may have broken
        const bool same =
            known && !failing && r.ip == last.ip && r.netmask == last.netmask && r.gateway == last.gateway;
        known = true;
        failing = false;
        last = r;
        return same ? Action::None : Action::Changed;
    }

private:
    bool known = false;    // a reading went to libtorrent
    bool failing = false;  // the last reading failed, and that was logged
    NetworkReading last;
};

}  // namespace torrent
