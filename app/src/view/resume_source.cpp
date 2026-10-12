#include "view/resume_source.hpp"
#include "view/video_card.hpp"
#include "view/context_menu.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/presenter.hpp"
#include "activity/player_view.hpp"
#include "tab/media_series.hpp"
#include "api/backend.hpp"
#include "utils/config.hpp"
#include <algorithm>
#include <ctime>

using namespace brls::literals;  // for _i18n

namespace {

std::string phraseText(const stremio::Phrase& p) {
    if (!p.key[0]) return "";
    if (p.args == 2) return brls::getStr(p.key, p.a, p.b);
    if (p.args == 1) return brls::getStr(p.key, p.a);
    return brls::getStr(p.key);
}

}  // namespace

ResumeDataSource::ResumeDataSource(const MediaList& items)
    : VideoDataSource(items), cards(items.size(), stremio::CardKind::AsSaved), resolved(items.size(), false) {}

ResumeDataSource::~ResumeDataSource() { this->endAsking(); }

void ResumeDataSource::endAsking() {
    if (!this->asking) return;
    this->asking = false;
    // never from inside the event's own fire (its callback only sets a flag)
    brls::Application::getGlobalFocusChangeEvent()->unsubscribe(this->focusWatch);
}

stremio::ResumeCard ResumeDataSource::card(size_t index) const {
    const media::Item& item = this->list[index];
    stremio::ResumeCard c;
    // a show whose saved episode id doesn't parse; anything else plays as it is, like a movie
    c.item = item.type == media::mediaTypeEpisode ? stremio::CardItem::Episode
             : item.type == media::mediaTypeShow  ? stremio::CardItem::Show
                                                  : stremio::CardItem::Movie;
    c.kind = this->cards[index];
    c.offsetMs = item.viewOffset;
    c.durationMs = item.duration;
    return c;
}

RecyclingGridItem* ResumeDataSource::cellForRow(RecyclingView* recycler, size_t index) {
    RecyclingGridItem* item = VideoDataSource::cellForRow(recycler, index);
    if (auto* cell = dynamic_cast<VideoCardCell*>(item)) this->decorate(cell, index);
    return item;
}

void ResumeDataSource::decorate(VideoCardCell* cell, size_t index) {
    if (index >= this->list.size()) return;
    const media::Item& item = this->list[index];
    const stremio::ResumeCard card = this->card(index);
    cell->setId(item.ratingKey);  // a finished episode's card may have become the next one
    cell->labelTitle->setVisibility(brls::Visibility::VISIBLE);
    cell->labelExt->setVisibility(brls::Visibility::VISIBLE);
    if (card.item == stremio::CardItem::Episode) {
        cell->labelTitle->setText(item.grandparentTitle);
        cell->labelExt->setText(stremio::episodeLabel(item.parentIndex, item.index, item.title));
    } else {
        cell->labelExt->setText("");  // a movie's second line is empty
    }
    // "32 min left" (or "Up next", "Played") over "2 days ago"
    cell->setCaption(phraseText(stremio::cardStatus(card)),
        phraseText(stremio::watchedAgo(item.lastViewedAt, int64_t(std::time(nullptr)))));
    // a progress bar only for what is in progress
    if (stremio::cardProgress(card)) {
        cell->rectProgress->setWidthPercentage(float(item.viewOffset) / float(item.duration) * 100.f);
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::VISIBLE);
    } else {
        cell->rectProgress->getParent()->setVisibility(brls::Visibility::GONE);
    }
    cell->badgeTopRight->setVisibility(brls::Visibility::GONE);
    // A plays at once (resumes, or the next episode), except where it opens the show's page
    const bool plays = stremio::cardAction(card, this->resolved[index]) != stremio::CardAction::OpenShow;
    cell->setPlayOverlay(plays);
    cell->updateActionHint(brls::BUTTON_A, plays ? "main/media/play"_i18n : "main/media/open"_i18n);
}

std::vector<size_t> ResumeDataSource::applyEpisodes(
    const std::string& showKey, const std::vector<media::Item>& episodes) {
    std::vector<size_t> changed;
    std::vector<stremio::EpisodeRef> refs;
    for (const auto& e : episodes) refs.push_back({e.parentIndex, e.index});
    for (size_t i = 0; i < this->list.size(); ++i) {
        media::Item& card = this->list[i];
        if (card.type != media::mediaTypeEpisode || card.grandparentRatingKey != showKey || this->resolved[i]) continue;
        this->resolved[i] = true;
        changed.push_back(i);
        for (const auto& e : episodes)
            if (e.parentIndex == card.parentIndex && e.index == card.index) {
                card.title = e.title;
                break;
            }
        const stremio::CardResolve r =
            stremio::resolveCard(refs, card.parentIndex, card.index, card.viewOffset, card.duration);
        this->cards[i] = r.kind;
        if (r.kind != stremio::CardKind::UpNext) continue;
        // the next episode, from its start; the show as the library names it (same poster)
        media::Item n = episodes[size_t(r.next)];
        n.grandparentRatingKey = card.grandparentRatingKey;
        n.grandparentTitle = card.grandparentTitle;
        n.grandparentThumb = card.grandparentThumb;
        n.lastViewedAt = card.lastViewedAt;
        n.viewOffset = 0;
        card = n;
    }
    return changed;
}

std::vector<std::string> ResumeDataSource::showKeys() const {
    std::vector<std::string> out;
    for (const auto& it : this->list)
        if (it.type == media::mediaTypeEpisode && !it.grandparentRatingKey.empty() &&
            std::find(out.begin(), out.end(), it.grandparentRatingKey) == out.end())
            out.push_back(it.grandparentRatingKey);
    return out;
}

void ResumeDataSource::onItemSelected(brls::Box* recycler, size_t index) {
    if (index >= this->list.size()) return;
    const media::Item item = this->list[index];
    switch (stremio::cardAction(this->card(index), this->resolved[index])) {
        case stremio::CardAction::Resume:
            play(item, 0);  // from its position
            return;
        case stremio::CardAction::PlayFromStart:
            play(item, PlayerView::START_OVER);  // the next episode, or a position under a second
            return;
        case stremio::CardAction::OpenShow:
            if (item.type != media::mediaTypeEpisode) {
                VideoDataSource::onItemSelected(recycler, index);  // a show with no episode saved: its page
            } else {
                // the show's last episode was finished, or the show no longer lists it: the show's page
                media::Item show;
                show.ratingKey = item.grandparentRatingKey;
                show.key = show.ratingKey;
                show.type = media::mediaTypeShow;
                show.title = item.grandparentTitle;
                show.thumb = item.grandparentThumb;
                ui::presentDetail(recycler, new MediaSeries(show));
            }
            return;
        case stremio::CardAction::AskEpisodes:
            break;
    }
    // a finished episode whose next one isn't known yet: ask the show, then decide again (once:
    // a second A while the answer is on its way does nothing). Only if the user is still where
    // A was pressed: any focus change meanwhile (another card or tab, the options panel or any
    // other activity pushed, even if closed again) drops it, so the player never opens over
    // what came next. The card then says what A does now.
    if (this->asking) return;
    this->asking = true;
    auto moved = std::make_shared<bool>(false);
    this->focusWatch =
        brls::Application::getGlobalFocusChangeEvent()->subscribe([moved](brls::View*) { *moved = true; });
    std::weak_ptr<bool> alive = this->token;
    const std::string showKey = item.grandparentRatingKey;
    AppConfig::instance().backend().getAllEpisodes(
        showKey, false,
        [this, alive, recycler, showKey, index, moved](const media::Container<media::Item>& r) {
            if (alive.expired()) return;  // the row was rebuilt meanwhile, this data source with it
            this->endAsking();
            auto* rv = dynamic_cast<RecyclingView*>(recycler);
            for (size_t i : this->applyEpisodes(showKey, r.Items))
                if (auto* cell = dynamic_cast<VideoCardCell*>(rv ? rv->getGridItemByIndex(i) : nullptr))
                    this->decorate(cell, i);
            // the focus never moved, the card still has it, and nothing sits above its activity
            brls::View* card = rv ? rv->getGridItemByIndex(index) : nullptr;
            const auto stack = brls::Application::getActivitiesStack();
            if (*moved || !card || !hasFocusWithin(card) || stack.empty() ||
                stack.back() != recycler->getParentActivity())
                return;
            this->onItemSelected(recycler, index);
        },
        [this, alive](const std::string& ex) {
            if (alive.expired()) return;
            this->endAsking();
            brls::Application::notify(ex);
        });
}

void ResumeDataSource::onContextMenu(brls::Box* recycler, size_t index) {
    if (index >= this->list.size()) return;
    brls::Box* menu = new ContextMenu(this->list[index], recycler, true);
    brls::Application::pushActivity(new brls::Activity(menu));
}

void ResumeDataSource::play(const media::Item& item, int64_t seekMs) {
    PlayerView* view = new PlayerView(item, seekMs);
    if (item.type == media::mediaTypeEpisode) {
        view->setTitie(item.title.empty()
                           ? fmt::format("{} \xC2\xB7 S{}E{}", item.grandparentTitle, item.parentIndex, item.index)
                           : fmt::format("{} \xC2\xB7 S{}E{} \xC2\xB7 {}", item.grandparentTitle, item.parentIndex,
                                 item.index, item.title));
        if (!item.grandparentRatingKey.empty()) view->setSeries(item.grandparentRatingKey);
    } else {
        view->setTitie(item.title);
    }
}
