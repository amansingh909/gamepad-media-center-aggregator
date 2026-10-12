// Continue Watching from the Stremio account library: which items the Home row lists, what a card
// says (time left, when last watched), where playback resumes, and which episode comes next.
// Header-only, free of nlohmann, borealis and curl (tests/test_resume.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace stremio {

/// Watched this share of its length, a movie or an episode counts as finished (the player's
/// scrobble threshold).
constexpr double finishedShare = 0.90;

/// One libraryItem's playback state (its `state` object).
struct LibraryState {
    int64_t timeOffsetMs = 0;
    int64_t durationMs = 0;
    bool flaggedWatched = false;
    std::string videoId;      // a series: the episode last played ("tt0903747:2:5"), `video_id`
    std::string lastWatched;  // ISO 8601, UTC ("2026-10-10T08:30:25.123Z")
};

/// One libraryItem as Continue Watching needs it.
struct LibraryEntry {
    std::string id;    // "tt0903747"
    std::string type;  // "movie" | "series" | "other"...
    std::string name;
    std::string poster;
    bool removed = false;  // not (or no longer) in the library list
    bool temp = false;     // watched without being added
    LibraryState state;
};

/// An episode's place in its show, for nextEpisodeIndex() and showNextUp().
struct EpisodeRef {
    int64_t season;
    int64_t episode;
};

/// An i18n key and the numbers its string takes (`args`: 0, 1 or 2 of a and b).
struct Phrase {
    const char* key;  // "" when there is nothing to say
    int args;
    int64_t a;
    int64_t b;
};

inline bool isFinished(int64_t offsetMs, int64_t durationMs) {
    return durationMs > 0 && double(offsetMs) >= finishedShare * double(durationMs);
}

/// Season and episode of a video id "{base}:{season}:{episode}"; false for any other id.
inline bool parseEpisodeId(const std::string& videoId, int64_t& season, int64_t& episode) {
    const size_t last = videoId.rfind(':');
    if (last == std::string::npos || last == 0) return false;
    const size_t mid = videoId.rfind(':', last - 1);
    if (mid == std::string::npos || mid == 0) return false;
    auto number = [](const std::string& s, int64_t& out) {
        if (s.empty() || s.size() > 6) return false;
        out = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    return number(videoId.substr(mid + 1, last - mid - 1), season) && number(videoId.substr(last + 1), episode);
}

/// Seconds since 1970 for an ISO 8601 UTC time ("2026-10-10T08:30:25.123Z"); 0 when it isn't one.
inline int64_t isoToEpochSeconds(const std::string& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (iso.size() < 19 || std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return 0;
    // days from the civil date (Howard Hinnant's algorithm)
    const int64_t yy = y - (mo <= 2 ? 1 : 0);
    const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    const int64_t yoe = yy - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

/// The items Continue Watching lists, most recently watched first, at most `count`: stremio-core's
/// rule (not "other", not taken out of the library on purpose, a saved position), except movies
/// watched to the end. A movie flagged watched but not finished stays (Stremio's clients flag at
/// 70%). A series stays with a finished episode (its card leads to the next one), and so does
/// one an older client flagged watched as a whole. Indexes into `items`.
inline std::vector<size_t> continueWatching(const std::vector<LibraryEntry>& items, size_t count) {
    std::vector<size_t> out;
    for (size_t i = 0; i < items.size(); ++i) {
        const LibraryState& s = items[i].state;
        if (items[i].type == "other" || (items[i].removed && !items[i].temp) || s.timeOffsetMs <= 0) continue;
        if (items[i].type == "movie" && isFinished(s.timeOffsetMs, s.durationMs)) continue;
        out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(),
        [&items](size_t a, size_t b) { return items[a].state.lastWatched > items[b].state.lastWatched; });
    if (out.size() > count) out.resize(count);
    return out;
}

/// Where a movie (`episodeVideoId` "") or the episode `episodeVideoId` resumes: the saved
/// position, or 0 when there is none, it is another episode's, or it was watched to the end. A
/// position under a second is the start (stremio-core saves 1 ms for the next episode).
inline int64_t resumePositionMs(const LibraryState& s, const std::string& episodeVideoId) {
    if (s.timeOffsetMs < 1000 || isFinished(s.timeOffsetMs, s.durationMs)) return 0;
    if (episodeVideoId.empty()) return s.timeOffsetMs;
    return s.videoId == episodeVideoId ? s.timeOffsetMs : 0;
}

/// "32 min left" or "1 h 4 min left", rounded up to the minute; key "" when the length isn't known.
inline Phrase timeLeft(int64_t offsetMs, int64_t durationMs) {
    if (durationMs <= 0) return {"", 0, 0, 0};
    const int64_t left = std::max<int64_t>(0, durationMs - offsetMs);
    const int64_t minutes = std::max<int64_t>(1, (left + 59999) / 60000);
    if (minutes < 60) return {"main/resume/min_left", 1, minutes, 0};
    return {"main/resume/hours_left", 2, minutes / 60, minutes % 60};
}

/// When it was last watched, from epoch seconds: "Just now", "5 min ago", "3 h ago",
/// "Yesterday" (24 to 48 hours ago), "2 days ago". Key "" when the time isn't known. A time a
/// little ahead of this clock (another device's) reads as "Just now".
inline Phrase watchedAgo(int64_t lastSecs, int64_t nowSecs) {
    if (lastSecs <= 0) return {"", 0, 0, 0};
    const int64_t d = std::max<int64_t>(0, nowSecs - lastSecs);
    if (d < 60) return {"main/resume/just_now", 0, 0, 0};
    if (d < 3600) return {"main/resume/min_ago", 1, d / 60, 0};
    if (d < 86400) return {"main/resume/hours_ago", 1, d / 3600, 0};
    const int64_t days = d / 86400;
    if (days == 1) return {"main/resume/yesterday", 0, 0, 0};
    return {"main/resume/days_ago", 1, days, 0};
}

/// The episode after (season, episode) in a show's episodes (sorted by season then episode,
/// as parseEpisodes() returns them), skipping specials (season 0); -1 when it was the last
/// one or isn't there.
inline int nextEpisodeIndex(const std::vector<EpisodeRef>& eps, int64_t season, int64_t episode) {
    for (size_t i = 0; i < eps.size(); ++i) {
        if (eps[i].season != season || eps[i].episode != episode) continue;
        for (size_t j = i + 1; j < eps.size(); ++j)
            if (eps[j].season > 0) return int(j);
        return -1;
    }
    return -1;
}

/// The episode a show page's Play button plays, from the show's saved state: the one in
/// progress, the one after a finished one, else the first (a show never played, or its last
/// episode finished: then `again` is true). -1 when `eps` is empty.
inline int showNextUp(const std::vector<EpisodeRef>& eps, const LibraryState& s, bool& again) {
    again = false;
    if (eps.empty()) return -1;
    int first = 0;
    for (size_t i = 0; i < eps.size(); ++i)
        if (eps[i].season > 0) {
            first = int(i);
            break;
        }
    int64_t season = 0, episode = 0;
    if (s.timeOffsetMs <= 0 || !parseEpisodeId(s.videoId, season, episode)) return first;
    int at = -1;
    for (size_t i = 0; i < eps.size(); ++i)
        if (eps[i].season == season && eps[i].episode == episode) {
            at = int(i);
            break;
        }
    if (at < 0) return first;
    if (!isFinished(s.timeOffsetMs, s.durationMs)) return at;
    const int next = nextEpisodeIndex(eps, season, episode);
    if (next >= 0) return next;
    again = true;
    return first;
}

/// What "Mark as watched" on one episode does to its show's saved state, as stremio-core does:
/// Advance to the next episode (its advance_to_video: position 1 ms, length unknown), so the
/// show stays in Continue Watching with that one; Clear the position when it was the last one or
/// the show doesn't list it (its mark_as_watched), so the show leaves the row; Keep the state
/// when the episode comes before the saved one, so marking an old episode never loses a later
/// one in progress, and when the show's episodes couldn't be fetched, so a network error never
/// wipes a position. (The player scrobbles before its stop report, so its own save of a
/// finished episode always lands after the mark.)
struct EpisodeWatched {
    enum Kind { Keep, Advance, Clear };
    Kind kind;
    int next;  // Advance: the next episode's index in the show's episodes; else -1
};

inline EpisodeWatched episodeWatched(
    const std::vector<EpisodeRef>& eps, const LibraryState& s, int64_t season, int64_t episode) {
    int64_t savedSeason = 0, savedEpisode = 0;
    if (parseEpisodeId(s.videoId, savedSeason, savedEpisode) &&
        (season < savedSeason || (season == savedSeason && episode < savedEpisode)))
        return {EpisodeWatched::Keep, -1};
    if (eps.empty()) return {EpisodeWatched::Keep, -1};
    const int next = nextEpisodeIndex(eps, season, episode);
    if (next >= 0) return {EpisodeWatched::Advance, next};
    return {EpisodeWatched::Clear, -1};
}

}  // namespace stremio
