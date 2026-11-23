// The Home tab's Continue Watching row for a Stremio account: each card shows the episode (for a
// show), the time left and when it was last watched; A resumes (a finished episode plays the next
// one); X opens the options (Start over, Mark played, Remove from list).
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <view/video_source.hpp>
#include <api/stremio/resume_card.hpp>

class VideoCardCell;

class ResumeDataSource : public VideoDataSource {
public:
    explicit ResumeDataSource(const MediaList& items);
    ~ResumeDataSource() override;

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override;
    void onItemSelected(brls::Box* recycler, size_t index) override;
    void onContextMenu(brls::Box* recycler, size_t index) override;

    /// The card's own lines over what VideoDataSource draws: "S2 E5 · Title", the caption
    /// ("32 min left" or "Up next", over "2 days ago") and the progress bar.
    void decorate(VideoCardCell* cell, size_t index);
    /// A show's episodes arrived (getAllEpisodes): its cards get their episode titles, and a
    /// finished episode's card turns into the next episode. The indexes of the cards it changed.
    std::vector<size_t> applyEpisodes(const std::string& showKey, const std::vector<media::Item>& episodes);
    /// The shows whose episodes the cards need, each once.
    std::vector<std::string> showKeys() const;
    /// Expires when this data source is deleted (the row rebuilt): callbacks check it first.
    std::weak_ptr<bool> alive() const { return this->token; }

    /// Plays a Continue Watching item, a movie or an episode, from its position (`seekMs` 0) or
    /// from the start (PlayerView::START_OVER), with the show's episodes for next and previous.
    static void play(const media::Item& item, int64_t seekMs);

private:
    stremio::ResumeCard card(size_t index) const;
    /// The wait of an A on a finished episode ends: its show's episodes arrived or failed, or
    /// this data source goes. The focus is no longer watched.
    void endAsking();

    std::vector<stremio::CardKind> cards;  // per item
    std::vector<bool> resolved;            // the show's episodes were applied to this card
    // A on a finished episode whose next one isn't known yet: its show's episodes are on their
    // way, and the focus is watched meanwhile (A is dropped if it moves)
    bool asking = false;
    brls::GenericEvent::Subscription focusWatch;
    std::shared_ptr<bool> token = std::make_shared<bool>(true);
};
