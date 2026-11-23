/*
    GMCA: the loading screen of a video, over the player until mpv has its first frame.
    The item's backdrop dimmed, its logo (or its title) pulsing slowly, a progress bar,
    the stage and a line of stats. For a torrent this device's engine fetches, the stage
    (Finding peers, Getting torrent info, Buffering 45%) and the peers and speed come from
    the engine (startLoading()); any other source, a link or a torrent on a streaming
    server, reports its own (startOpening()). B cancels. Built only with the torrent engine
    (USE_TORRENT).
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
    /// What a source other than this device's engine says while it opens: the stage line, the
    /// bar's fill (0 to 1; below 0, a sweep for a wait it can't measure) and the line under it
    /// ("" for none).
    struct Report {
        std::string stage;
        float progress = -1;
        std::string stats;
    };
    /// Shows the screen for `item`, with what `report` says, asked a few times a second.
    void startOpening(const media::Item& item, std::function<Report()> report);
    void stopLoading();
    bool isLoading() const { return this->shown; }
    /// What B does (the player closes).
    void setCancelAction(std::function<void()> action) { this->cancelAction = std::move(action); }
    /// While it shows, the d-pad goes nowhere: the player under it stays out of reach.
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

private:
    void show(const media::Item& item);
    void update();

    BRLS_BIND(brls::Image, backdrop, "torrent/backdrop");
    BRLS_BIND(brls::Image, logo, "torrent/logo");
    BRLS_BIND(brls::Label, title, "torrent/title");
    BRLS_BIND(brls::Box, barFill, "torrent/bar/fill");
    BRLS_BIND(brls::Label, stage, "torrent/stage");
    BRLS_BIND(brls::Label, stats, "torrent/stats");

    brls::RepeatingTimer ticker;
    std::shared_ptr<torrent::Warmup> warmup;
    std::function<Report()> report;  // startOpening()'s
    Report lastReport;               // its last answer, between the text updates
    std::function<void()> cancelAction;
    std::chrono::steady_clock::time_point shownAt;
    int ticks = 0;
    bool shown = false;
};

#endif
