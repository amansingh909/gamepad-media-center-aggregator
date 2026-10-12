#include "view/torrent_loading.hpp"

#ifdef USE_TORRENT

#include <torrent/torrent_service.hpp>
#include <torrent/ui_model.hpp>
#include "utils/image.hpp"

using namespace brls::literals;

namespace {
const float barWidth = 420;      // the bar's width in torrent_loading.xml
const brls::Time tickMs = 50;    // the pulse moves every tick, the text every fifth
}  // namespace

TorrentLoadingView::TorrentLoadingView() {
    this->inflateFromXMLRes("xml/view/torrent_loading.xml");
    this->setFocusable(true);
    this->setHideHighlightBorder(true);
    this->setHideHighlightBackground(true);
    this->setHideClickAnimation(true);
    this->setVisibility(brls::Visibility::GONE);
    this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [this](brls::View*) {
        if (this->cancelAction) this->cancelAction();
        return true;
    });
    this->ticker.setCallback([this]() { this->update(); });
}

TorrentLoadingView::~TorrentLoadingView() {
    this->ticker.stop();
    Image::cancel(this->backdrop);
    Image::cancel(this->logo);
}

void TorrentLoadingView::startLoading(const media::Item& item, std::shared_ptr<torrent::Warmup> warmup) {
    this->warmup = std::move(warmup);
    const std::string& art = item.art.empty() ? item.grandparentArt : item.art;
    Image::cancel(this->backdrop);
    if (!art.empty()) Image::load(this->backdrop, art, 1280, 720);
    Image::cancel(this->logo);
    if (!item.clearLogo.empty()) {
        this->logo->setVisibility(brls::Visibility::VISIBLE);
        this->title->setVisibility(brls::Visibility::GONE);
        Image::load(this->logo, item.clearLogo, 440, 120);
    } else {
        this->logo->setVisibility(brls::Visibility::GONE);
        this->title->setVisibility(brls::Visibility::VISIBLE);
        this->title->setText(item.grandparentTitle.empty() ? item.title : item.grandparentTitle);
    }
    this->shownAt = std::chrono::steady_clock::now();
    this->ticks = 0;
    this->shown = true;
    this->setVisibility(brls::Visibility::VISIBLE);
    this->update();
    this->ticker.start(tickMs);
}

void TorrentLoadingView::stopLoading() {
    this->ticker.stop();
    this->warmup.reset();
    this->shown = false;
    this->setVisibility(brls::Visibility::GONE);
}

void TorrentLoadingView::update() {
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - this->shownAt).count();
    const float alpha = torrent::pulseAlpha(secs);
    this->logo->setAlpha(alpha);
    this->title->setAlpha(alpha);
    if (this->ticks++ % 5 != 0) return;

    auto* service = torrent::TorrentService::current();
    const torrent::Stats engineStats = service ? service->snapshot() : torrent::Stats{};
    const int64_t buffered = this->warmup ? this->warmup->buffered() : 0;
    const int64_t target = this->warmup ? this->warmup->target() : 0;
    const torrent::LoadingState s = torrent::loadingState(engineStats, buffered, target);
    this->barFill->setWidth(barWidth * s.progress);
    switch (s.stage) {
        case torrent::LoadingStage::FindingPeers:
            this->stage->setText("main/torrent/loading/finding_peers"_i18n);
            break;
        case torrent::LoadingStage::GettingInfo:
            this->stage->setText("main/torrent/loading/getting_info"_i18n);
            break;
        case torrent::LoadingStage::Buffering:
            this->stage->setText(brls::getStr("main/torrent/loading/buffering", s.percent));
            break;
    }
    this->stats->setText(brls::getStr("main/torrent/loading/peers", s.peers) + "  \xC2\xB7  " +
                         torrent::formatRate(s.bytesPerSec));
}

#endif
