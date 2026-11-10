// When the source list prefetches a torrent: once its row has kept the focus for a second
// (scrolling past rows prefetches nothing), and not again for 90 s (the engine keeps a
// prefetched torrent two minutes). Header-only (tests/test_torrent_prefetch.cpp).
#pragma once

#include <map>
#include <string>

namespace torrent {

class PrefetchGate {
public:
    explicit PrefetchGate(double delaySecs = 1.0, double repeatSecs = 90.0) : delay(delaySecs), repeat(repeatSecs) {}

    /// A torrent row got the focus at `now` (seconds, any steady clock).
    void focus(const std::string& key, double now) {
        focused = key;
        since = now;
    }

    /// That row lost it. True when it was the one waiting (focus can move to the next row
    /// before the last one hears it lost it).
    bool blur(const std::string& key) {
        if (focused != key) return false;
        focused.clear();
        return true;
    }

    /// The key to prefetch now, at most once per focus, or "". A frame timer can fire a
    /// frame early, hence the 50 ms slack.
    std::string due(double now) {
        if (focused.empty() || now - since < delay - 0.05) return "";
        auto it = done.find(focused);
        if (it != done.end() && now - it->second < repeat) return "";
        done[focused] = now;
        std::string key = focused;
        focused.clear();
        return key;
    }

    /// The key may be prefetched again at its next focus: it just played, and a prefetch
    /// then keeps its torrent in the engine for a replay.
    void forget(const std::string& key) { done.erase(key); }

private:
    double delay, repeat;
    std::string focused;
    double since = 0;
    std::map<std::string, double> done;  // key: when it was last prefetched
};

}  // namespace torrent
