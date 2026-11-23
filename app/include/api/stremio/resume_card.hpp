// A Continue Watching card on the Home row (view/resume_source.cpp): what it stands for once its
// show's episodes are known, its second line, its caption and progress bar, and what A does.
// Header-only, free of nlohmann, borealis and curl (tests/test_resume_card.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "api/stremio/resume.hpp"

namespace stremio {

/// What a card stands for: the item as the library saved it (AsSaved), the episode after a
/// finished one (UpNext), or a finished episode with none after it in its show (LastDone).
enum class CardKind { AsSaved, UpNext, LastDone };

/// A card's item: a movie, an episode, or a show whose saved episode id doesn't parse.
enum class CardItem { Movie, Episode, Show };

struct ResumeCard {
    CardItem item = CardItem::Movie;
    CardKind kind = CardKind::AsSaved;
    int64_t offsetMs = 0;
    int64_t durationMs = 0;
};

/// A card once its show's episodes are known; `next` is the next episode's index for UpNext,
/// else -1.
struct CardResolve {
    CardKind kind;
    int next;
};

/// What A does on a card.
enum class CardAction {
    Resume,         // play from the saved position
    PlayFromStart,  // play from 0
    OpenShow,       // the show's page
    AskEpisodes,    // a finished episode whose next one isn't known yet: ask the show first
};

/// A show's second line: "S2 E5 · Title", or "S2 E5" before the show's episodes arrive.
inline std::string episodeLabel(int64_t season, int64_t episode, const std::string& title) {
    std::string out = "S" + std::to_string(season) + " E" + std::to_string(episode);
    if (!title.empty()) out += " \xC2\xB7 " + title;
    return out;
}

/// The episode (season, episode) at this position, once its show's episodes (sorted, as
/// parseEpisodes() returns them) are known: as saved while it isn't finished, else the next
/// episode, else the last one done (the last of the show, or one it no longer lists).
inline CardResolve resolveCard(
    const std::vector<EpisodeRef>& eps, int64_t season, int64_t episode, int64_t offsetMs, int64_t durationMs) {
    if (!isFinished(offsetMs, durationMs)) return {CardKind::AsSaved, -1};
    const int next = nextEpisodeIndex(eps, season, episode);
    if (next >= 0) return {CardKind::UpNext, next};
    return {CardKind::LastDone, -1};
}

/// Finished as saved: the next episode isn't known yet (or it's a movie the row shouldn't list).
inline bool cardFinished(const ResumeCard& c) {
    return c.kind == CardKind::AsSaved && isFinished(c.offsetMs, c.durationMs);
}

/// The caption's first line: "Up next", "Played" for a finished episode, else the time left
/// ("32 min left"). Key "" when there is nothing to say: no length known (a show just advanced
/// to its next episode saves none), or a show with no episode saved.
inline Phrase cardStatus(const ResumeCard& c) {
    if (c.item == CardItem::Show) return {"", 0, 0, 0};
    if (c.kind == CardKind::UpNext) return {"main/resume/up_next", 0, 0, 0};
    if (c.kind == CardKind::LastDone || cardFinished(c)) return {"main/media/played", 0, 0, 0};
    return timeLeft(c.offsetMs, c.durationMs);
}

/// A progress bar only for what is in progress: a position of a second or more (stremio-core
/// saves 1 ms for the start of the next episode), a known length, not finished.
inline bool cardProgress(const ResumeCard& c) {
    return c.item != CardItem::Show && c.kind == CardKind::AsSaved && !cardFinished(c) && c.offsetMs >= 1000 &&
           c.durationMs > 0;
}

/// What A does: a movie or an episode resumes from its position (from the start under a
/// second), the next episode plays from the start, a show whose last episode was finished (or
/// with no episode saved) opens its page, and a finished episode whose next one isn't known
/// yet asks its show first. `resolved`: the show's episodes were applied to this card.
inline CardAction cardAction(const ResumeCard& c, bool resolved) {
    if (c.item == CardItem::Show) return CardAction::OpenShow;
    if (c.kind == CardKind::UpNext) return CardAction::PlayFromStart;
    if (c.kind == CardKind::AsSaved && !cardFinished(c))
        return c.offsetMs >= 1000 ? CardAction::Resume : CardAction::PlayFromStart;
    if (c.item == CardItem::Movie) return CardAction::PlayFromStart;  // watched to the end
    if (c.kind == CardKind::LastDone || resolved) return CardAction::OpenShow;
    return CardAction::AskEpisodes;
}

}  // namespace stremio
