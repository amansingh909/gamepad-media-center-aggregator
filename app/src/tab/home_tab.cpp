#include "tab/home_tab.hpp"
#include "view/recyling_video.hpp"
#include "view/loading_spinner.hpp"
#include "view/resume_source.hpp"
#include "view/video_card.hpp"
#include "api/plex.hpp"
#include "api/backend.hpp"
#include "utils/keybind.hpp"
#include "utils/network_state.hpp"
#include "utils/offline_library.hpp"
#include "utils/offline_ui.hpp"
#include "utils/session_cache.hpp"

using namespace brls::literals;  // for _i18n

HomeTab::HomeTab() {
    brls::Logger::debug("Tab HomeTab: create");
    this->inflateFromXMLRes("xml/tabs/home.xml");
    // centered spinner overlay shown while the home hubs load (the rows are
    // built into boxHome only once the response arrives).
    this->spinner = new LoadingSpinner();
    this->addView(this->spinner);
}

HomeTab::~HomeTab() { brls::Logger::debug("View HomeTab: delete"); }

brls::View* HomeTab::create() { return new HomeTab(); }

/// The home screen mirrors the rows configured server-side: "Continue
/// watching" then the hubs from /hubs, with their localized titles
/// (X-Plex-Language) — PLEX_MIGRATION.md §2.5.
void HomeTab::doRequest() {
    // drop any previous offline empty overlay -> back to the scrollable list
    if (this->offlineEmpty) {
        this->removeView(this->offlineEmpty);
        this->offlineEmpty = nullptr;
        this->scroll->setVisibility(brls::Visibility::VISIBLE);
    }

    // offline: the server hubs are unavailable — show one poster row per
    // downloaded library instead (same row layout as online) (SPEC AC13)
    if (NetworkState::isOffline()) {
        this->boxHome->clearViews();
        auto& lib = OfflineLibrary::instance();
        bool any = false;
        for (auto& s : lib.sections()) {
            auto items = lib.sectionItems(s.key);
            if (items.empty()) continue;
            RecylingVideo* row = new RecylingVideo();
            row->setTitle(s.title);
            row->setFrameHeight(brls::getStyle()["app/card/poster/row"]);
            row->setItemWidth(brls::getStyle()["app/card/poster/width"]);
            row->setSidePadding(brls::getStyle()["main/content_padding_sides"]);
            row->setItems(items);
            this->boxHome->addView(row);
            any = true;
        }
        // nothing downloaded: offline empty state (icon + message + Retry),
        // centered by filling the tab (scroll hidden) instead of scrolling
        if (!any) {
            this->scroll->setVisibility(brls::Visibility::GONE);
            this->offlineEmpty = offline_ui::makeEmpty();
            this->addView(this->offlineEmpty);
        }
        return;
    }

    // clearViews destroys the focused card when refreshing after playback:
    // remember to give the focus back to the first rebuilt row, otherwise it
    // falls back to the sidebar and the user loses track of it
    this->restoreFocus = hasFocusWithin(this);
    this->spinner->setSpinning(true);
    this->boxHome->clearViews();

    // "Continue watching" row — added right away to guarantee its position
    // at the top, filled when the response arrives
    RecylingVideo* resume = new RecylingVideo();
    resume->setTitle("main/home/resume"_i18n);
    resume->setFrameHeight(brls::getStyle()["app/card/poster/row"]);
    resume->setItemWidth(brls::getStyle()["app/card/poster/width"]);
    resume->setSidePadding(brls::getStyle()["main/content_padding_sides"]);
    resume->setVisibility(brls::Visibility::GONE);
    this->boxHome->addView(resume);

    this->doResume(resume);
    this->doHubs();
}

void HomeTab::doResume(RecylingVideo* row) {
    ASYNC_RETAIN
    AppConfig::instance().backend().getContinueWatching(20,
        [ASYNC_TOKEN, row](const media::Container<media::Hub>& r) {
            ASYNC_RELEASE
            for (auto& hub : r.Items) {
                if (hub.items.empty()) continue;
                std::string title = hub.title.empty() ? "main/home/resume"_i18n : hub.title;
                row->setTitle(title);
                if (AppConfig::instance().backend().type() == media::BackendType::Stremio) {
                    // Stremio: cards with the episode, the time left and when last watched
                    auto* source = new ResumeDataSource(hub.items);
                    row->setSource(source);
                    this->fillEpisodes(row, source);
                } else if (hub.more && !hub.key.empty()) {
                    // truncated hub (more=1): "+" card to the full page
                    row->setItems(hub.items, title, hub.key);
                } else {
                    row->setItems(hub.items);
                }
                this->tryRestoreFocus();
                return;
            }
            row->setItems({});
        },
        [ASYNC_TOKEN, row](const std::string& ex) {
            ASYNC_RELEASE
            row->setItems({});
            brls::Logger::warning("home continueWatching: {}", ex);
        });
}

void HomeTab::doHubs() {
    ASYNC_RETAIN
    AppConfig::instance().backend().getHomeHubs(20, true,
        [ASYNC_TOKEN](const media::Container<media::Hub>& r) {
            ASYNC_RELEASE
            for (auto& hub : r.Items) {
                if (hub.items.empty()) continue;
                // in case the server returns them despite excludeContinueWatching
                if (hub.hubIdentifier == "home.continue" || hub.hubIdentifier == "home.ondeck") continue;

                // playlist hubs mix audio/photo/video: only video playlists
                // are playable in pleNx
                std::vector<plex::Item> items;
                items.reserve(hub.items.size());
                for (auto& item : hub.items) {
                    if (item.type == plex::mediaTypePlaylist && item.playlistType != "video") continue;
                    items.push_back(item);
                }
                if (items.empty()) continue;

                RecylingVideo* row = new RecylingVideo();
                row->setTitle(hub.title);
                float frameHeight = brls::getStyle()["app/card/poster/row"];
                row->setFrameHeight(frameHeight);
                // playlists AND music (artist/album/track): SQUARE covers (1:1)
                // — width = image height of the row (frame - 55 of labels,
                // video_card.xml metrics); everything else keeps 2:3 posters
                const std::string& t0 = items.front().type;
                bool square = t0 == plex::mediaTypePlaylist || t0 == plex::mediaTypeArtist ||
                              t0 == plex::mediaTypeAlbum || t0 == plex::mediaTypeTrack;
                row->setItemWidth(square ? frameHeight - 55 : brls::getStyle()["app/card/poster/width"]);
                row->setSidePadding(brls::getStyle()["main/content_padding_sides"]);
                // truncated hub (more=1): "+" card to the full page
                if (hub.more && !hub.key.empty()) {
                    row->setItems(items, hub.title, hub.key);
                } else {
                    row->setItems(items);
                }
                this->boxHome->addView(row);
            }
            this->spinner->setSpinning(false);
            this->tryRestoreFocus();
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            this->spinner->setSpinning(false);
            auto dialog = new brls::Dialog(ex);
            dialog->addButton("hints/retry"_i18n, [this]() { brls::sync([this]() { this->doRequest(); }); });
            dialog->addButton("hints/cancel"_i18n, []() {});
            dialog->open();
        });
}

void HomeTab::fillEpisodes(RecylingVideo* row, ResumeDataSource* source) {
    // Every player close rebuilds this row (Presenter), Home shown or not. The backend keeps each
    // show's episodes for the session: kept shows apply at once, and the others are asked one at
    // a time, so a replay or a page opened meanwhile waits for one request at most on the async
    // worker, not for one per show.
    using Episodes = std::vector<media::Item>;
    std::weak_ptr<bool> alive = source->alive();
    cache::walkOneAtATime<Episodes>(
        source->showKeys(),
        [](const std::string& show) { return AppConfig::instance().backend().keptEpisodes(show); },
        [](const std::string& show, cache::SessionCache<Episodes>::Answer answer) {
            AppConfig::instance().backend().getAllEpisodes(
                show, false, [answer](const media::Container<media::Item>& r) { answer(&r.Items, ""); },
                [answer](const std::string& ex) { answer(nullptr, ex); });
        },
        // the row was rebuilt meanwhile, its data source with it: stop there
        [alive]() { return !alive.expired(); },
        [source, row](const std::string& show, const Episodes* eps, const std::string& ex) {
            if (!eps) {
                brls::Logger::warning("continue watching {}: {}", show, ex);
                return;
            }
            for (size_t i : source->applyEpisodes(show, *eps))
                if (auto* cell = dynamic_cast<VideoCardCell*>(row->visibleCard(i))) source->decorate(cell, i);
        });
}

void HomeTab::tryRestoreFocus() {
    if (!this->restoreFocus) return;
    this->restoreFocus = false;
    // deferred one frame: the rows added by this callback are not laid out
    // yet, and giveFocus before the first layout fails silently
    ASYNC_RETAIN
    brls::sync([ASYNC_TOKEN]() {
        ASYNC_RELEASE
        brls::View* target = this->boxHome->getDefaultFocus();
        if (target) brls::Application::giveFocus(target);
    });
}

void HomeTab::onCreate() {
    auto actionRefresh = [this](brls::View* view) {
        this->doRequest();
        return true;
    };

    this->registerAction("hints/refresh"_i18n, brls::BUTTON_BACK, actionRefresh);
    this->registerAction(KeyBind::getRefresh(), actionRefresh);

    this->doRequest();
}
