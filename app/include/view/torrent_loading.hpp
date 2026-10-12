/*
    GMCA: the loading screen of a torrent source, over the player until mpv has data.
    The item's backdrop dimmed, its logo (or its title) pulsing slowly, a progress bar,
    the stage (Finding peers, Getting torrent info, Buffering 45%) and the peers and
    speed. B cancels. Built only with the torrent engine (USE_TORRENT).
*/

#pragma once

#ifdef USE_TORRENT

#include <borealis.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <api/media/types.hpp>

namespace torrent {
class Warmup;
}

class TorrentLoadingView : public brls::Box {
public:
    TorrentLoadingView();
    ~TorrentLoadingView() override;

    /// Shows the screen for `item`, following `warmup`'s progress and the watched torrent's
    /// stats (TorrentService::watch()).
    void startLoading(const media::Item& item, std::shared_ptr<torrent::Warmup> warmup);
    void stopLoading();
    bool isLoading() const { return this->shown; }
    /// What B does (the player closes).
    void setCancelAction(std::function<void()> action) { this->cancelAction = std::move(action); }

private:
    void update();

    BRLS_BIND(brls::Image, backdrop, "torrent/backdrop");
    BRLS_BIND(brls::Image, logo, "torrent/logo");
    BRLS_BIND(brls::Label, title, "torrent/title");
    BRLS_BIND(brls::Box, barFill, "torrent/bar/fill");
    BRLS_BIND(brls::Label, stage, "torrent/stage");
    BRLS_BIND(brls::Label, stats, "torrent/stats");

    brls::RepeatingTimer ticker;
    std::shared_ptr<torrent::Warmup> warmup;
    std::function<void()> cancelAction;
    std::chrono::steady_clock::time_point shownAt;
    int ticks = 0;
    bool shown = false;
};

#endif
