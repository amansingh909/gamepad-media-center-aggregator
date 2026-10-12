// Standalone logic test: Continue Watching from the Stremio library (which items, in what order),
// resume positions, the cards' texts, and the next episode.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_resume.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <vector>
#include <api/stremio/resume.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

using namespace stremio;

static LibraryEntry entry(const std::string& id, const std::string& type, int64_t offsetMin, int64_t durationMin,
                          const std::string& lastWatched, const std::string& videoId = "", bool flagged = false) {
    LibraryEntry e;
    e.id = id;
    e.type = type;
    e.name = id;
    e.state.timeOffsetMs = offsetMin * 60000;
    e.state.durationMs = durationMin * 60000;
    e.state.lastWatched = lastWatched;
    e.state.videoId = videoId;
    e.state.flaggedWatched = flagged;
    return e;
}

int main() {
    // ISO times
    CHECK(isoToEpochSeconds("2026-10-10T08:30:25.123Z") == 1791621025LL);
    CHECK(isoToEpochSeconds("2000-02-29T23:59:59Z") == 951868799LL);
    CHECK(isoToEpochSeconds("2026-10-12T00:00:00.000Z") == 1791763200LL);
    CHECK(isoToEpochSeconds("") == 0);
    CHECK(isoToEpochSeconds("yesterday") == 0);
    CHECK(isoToEpochSeconds("2026-13-01T00:00:00Z") == 0);

    // episode ids
    int64_t season = 0, episode = 0;
    CHECK(parseEpisodeId("tt0903747:2:5", season, episode) && season == 2 && episode == 5);
    CHECK(!parseEpisodeId("tt0903747", season, episode));
    CHECK(!parseEpisodeId("tt0903747:x:5", season, episode));
    CHECK(!parseEpisodeId("", season, episode));

    // which items, in what order: newest first; finished movies out; series stay; a movie flagged
    // watched but not finished stays (Stremio flags at 70%); stremio-core's rule otherwise: no
    // "other", nothing taken out of the library on purpose (removed and not temp)
    LibraryEntry dropped = entry("dropped", "series", 10, 44, "2026-10-11T23:30:00Z", "tt4:1:1");
    dropped.removed = true;
    LibraryEntry watchedOnly = entry("watchedOnly", "movie", 30, 90, "2026-10-11T23:40:00Z");
    watchedOnly.removed = true;  // watched without adding it to the library
    watchedOnly.temp = true;
    std::vector<LibraryEntry> lib = {
        entry("movieHalf", "movie", 40, 90, "2026-10-11T10:00:00Z"),
        entry("showNow", "series", 10, 44, "2026-10-11T20:00:00Z", "tt1:1:2"),
        entry("movieFlagged", "movie", 80, 90, "2026-10-11T21:00:00Z", "", true),
        entry("movieEnd", "movie", 85, 90, "2026-10-11T22:00:00Z"),
        entry("showDoneEp", "series", 43, 44, "2026-10-10T08:00:00Z", "tt2:3:7"),
        entry("showLegacy", "series", 20, 44, "2026-10-09T08:00:00Z", "tt3:1:1", true),
        entry("neverPlayed", "movie", 0, 90, "2026-10-11T23:00:00Z"),
        entry("noState", "series", 0, 0, ""),
        dropped,
        watchedOnly,
        entry("channel", "other", 10, 0, "2026-10-11T23:50:00Z"),
    };
    std::vector<size_t> rows = continueWatching(lib, 20);
    std::string order;
    for (size_t i : rows) order += (order.empty() ? "" : " ") + lib[i].id;
    CHECK_EQ(order, "watchedOnly movieFlagged showNow movieHalf showDoneEp showLegacy");
    CHECK(continueWatching(lib, 2).size() == 2);
    CHECK(continueWatching({}, 20).empty());

    // resume positions
    LibraryState s;
    s.timeOffsetMs = 40 * 60000;
    s.durationMs = 90 * 60000;
    CHECK(resumePositionMs(s, "") == 40 * 60000);
    s.flaggedWatched = true;  // flagged watched, not finished: a rewatch resumes too
    CHECK(resumePositionMs(s, "") == 40 * 60000);
    s.flaggedWatched = false;
    s.timeOffsetMs = 85 * 60000;  // watched to the end: from the start
    CHECK(resumePositionMs(s, "") == 0);
    s.timeOffsetMs = 95 * 60000;  // a position past the length: from the start too
    CHECK(resumePositionMs(s, "") == 0);
    s.timeOffsetMs = 10 * 60000;
    s.videoId = "tt1:1:2";
    CHECK(resumePositionMs(s, "tt1:1:2") == 10 * 60000);
    CHECK(resumePositionMs(s, "tt1:1:3") == 0);
    s.flaggedWatched = true;  // an older client flagged the whole show: the episode still resumes
    CHECK(resumePositionMs(s, "tt1:1:2") == 10 * 60000);
    s.durationMs = 0;  // no length known: the position as saved
    CHECK(resumePositionMs(s, "tt1:1:2") == 10 * 60000);
    CHECK(resumePositionMs(LibraryState{}, "") == 0);
    LibraryState advanced;  // stremio-core's advance_to_video: the next episode at 1 ms
    advanced.timeOffsetMs = 1;
    advanced.videoId = "tt1:1:3";
    CHECK(resumePositionMs(advanced, "tt1:1:3") == 0);  // under a second: from the start

    // time left
    Phrase left = timeLeft(30 * 60000, 94 * 60000);
    CHECK_EQ(left.key, "main/resume/hours_left");
    CHECK(left.args == 2 && left.a == 1 && left.b == 4);
    left = timeLeft(90 * 60000, 120 * 60000);
    CHECK_EQ(left.key, "main/resume/min_left");
    CHECK(left.args == 1 && left.a == 30);
    CHECK(timeLeft(60 * 60000 - 1, 60 * 60000).a == 1);  // under a minute left: 1 min
    CHECK(timeLeft(100 * 60000, 90 * 60000).a == 1);      // past the end: 1 min, never 0
    CHECK_EQ(timeLeft(10 * 60000, 0).key, "");

    // last watched
    const int64_t now = 1791763200;  // 2026-10-12T00:00:00Z
    CHECK_EQ(watchedAgo(now - 30, now).key, "main/resume/just_now");
    CHECK_EQ(watchedAgo(now + 120, now).key, "main/resume/just_now");
    CHECK_EQ(watchedAgo(now - 300, now).key, "main/resume/min_ago");
    CHECK(watchedAgo(now - 300, now).a == 5);
    CHECK_EQ(watchedAgo(now - 3 * 3600, now).key, "main/resume/hours_ago");
    CHECK(watchedAgo(now - 3 * 3600, now).a == 3);
    CHECK_EQ(watchedAgo(1791621025, now).key, "main/resume/yesterday");  // 39 h ago
    CHECK_EQ(watchedAgo(now - 2 * 86400 - 60, now).key, "main/resume/days_ago");
    CHECK(watchedAgo(now - 2 * 86400 - 60, now).a == 2);
    CHECK_EQ(watchedAgo(0, now).key, "");

    // the next episode, skipping specials
    std::vector<EpisodeRef> eps = {{0, 1}, {1, 1}, {1, 2}, {2, 1}};
    CHECK(nextEpisodeIndex(eps, 1, 2) == 3);
    CHECK(nextEpisodeIndex(eps, 1, 1) == 2);
    CHECK(nextEpisodeIndex(eps, 0, 1) == 1);
    CHECK(nextEpisodeIndex(eps, 2, 1) == -1);
    CHECK(nextEpisodeIndex(eps, 5, 5) == -1);

    // the show page's Play button
    bool again = true;
    CHECK(showNextUp(eps, LibraryState{}, again) == 1 && !again);  // never played: S1E1, not the special
    LibraryState show;
    show.durationMs = 44 * 60000;
    show.timeOffsetMs = 10 * 60000;
    show.videoId = "tt1:1:2";
    CHECK(showNextUp(eps, show, again) == 2 && !again);  // in progress
    show.timeOffsetMs = 43 * 60000;
    CHECK(showNextUp(eps, show, again) == 3 && !again);  // finished: the next one
    show.videoId = "tt1:2:1";
    CHECK(showNextUp(eps, show, again) == 1 && again);   // the last one finished: from the top
    show.videoId = "tt1:9:9";
    CHECK(showNextUp(eps, show, again) == 1 && !again);  // an episode the show no longer lists
    CHECK(showNextUp({}, show, again) == -1);
    show.durationMs = 0;  // advanced to the next episode: that one, not again
    show.timeOffsetMs = 1;
    show.videoId = "tt1:2:1";
    CHECK(showNextUp(eps, show, again) == 3 && !again);

    // "Mark as watched" on an episode, as stremio-core does
    LibraryState saved;  // S1E2 in progress
    saved.timeOffsetMs = 10 * 60000;
    saved.durationMs = 44 * 60000;
    saved.videoId = "tt1:1:2";
    EpisodeWatched w = episodeWatched(eps, saved, 1, 2);
    CHECK(w.kind == EpisodeWatched::Advance && w.next == 3);  // the next one, into the next season
    w = episodeWatched(eps, LibraryState{}, 1, 1);
    CHECK(w.kind == EpisodeWatched::Advance && w.next == 2);  // nothing saved yet: advances too
    w = episodeWatched(eps, saved, 2, 1);
    CHECK(w.kind == EpisodeWatched::Clear && w.next == -1);  // the last one: the show leaves the row
    w = episodeWatched(eps, saved, 1, 1);
    CHECK(w.kind == EpisodeWatched::Keep);  // before the saved one: S1E2 stays where it was
    w = episodeWatched(eps, saved, 0, 1);
    CHECK(w.kind == EpisodeWatched::Keep);  // a special sorts first: kept too
    w = episodeWatched(eps, saved, 5, 5);
    CHECK(w.kind == EpisodeWatched::Clear);  // an episode the show doesn't list
    w = episodeWatched({}, saved, 2, 1);
    CHECK(w.kind == EpisodeWatched::Clear);  // the show's episodes unknown

    if (failures == 0) printf("test_resume: all passed\n");
    return failures ? 1 : 0;
}
