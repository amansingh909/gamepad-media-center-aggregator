// Standalone logic test: a Stremio account libraryItem's JSON as GMCA reads it (stremio-core's
// keys, `state.video_id`; an older GMCA build's `state.videoId`) and the state changes GMCA saves.
//
//   c++ -std=gnu++17 -Wall -Iapp/include -Ilibrary/borealis/library/include/borealis/extern tests/test_library_item.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <vector>
#include <api/stremio/library_item.hpp>

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
using nlohmann::json;

// A series as stremio-core saves it (src/types/library/library_item.rs): camelCase keys, except
// the state's `video_id`.
static const char* stremioSeries = R"({
  "_id": "tt0903747", "name": "Breaking Bad", "type": "series",
  "poster": "https://images.metahub.space/poster/medium/tt0903747/img", "posterShape": "poster",
  "removed": false, "temp": false,
  "_ctime": "2026-09-01T10:00:00.000Z", "_mtime": "2026-10-10T08:31:00.000Z",
  "state": {
    "lastWatched": "2026-10-10T08:30:25.123Z", "timeWatched": 1200000, "timeOffset": 1500000,
    "overallTimeWatched": 9000000, "timesWatched": 0, "flaggedWatched": 0, "duration": 2820000,
    "video_id": "tt0903747:2:5", "watched": "tt0903747:2:4:5:eJyrVkrOz0nVUcrJzEtVUsorzcmpBQA", "noNotif": false
  },
  "behaviorHints": {"defaultVideoId": null, "featuredVideoId": null, "hasScheduledVideos": false}
})";

// A movie watched without adding it (removed and temp), which a Stremio client flagged watched
// at 70%.
static const char* stremioMovie = R"({
  "_id": "tt0013442", "name": "Nosferatu", "type": "movie", "poster": "", "posterShape": "poster",
  "removed": true, "temp": true, "_ctime": "2026-10-01T20:00:00.000Z", "_mtime": "2026-10-01T21:10:00.000Z",
  "state": {
    "lastWatched": "2026-10-01T21:10:00.000Z", "timeWatched": 4000000, "timeOffset": 4000000,
    "overallTimeWatched": 4000000, "timesWatched": 1, "flaggedWatched": 1, "duration": 5340000,
    "video_id": "tt0013442", "watched": null, "noNotif": false
  },
  "behaviorHints": {}
})";

// An older GMCA build's series: `videoId`, no `video_id`.
static const char* legacySeries = R"({
  "_id": "tt1", "name": "Old", "type": "series", "removed": true, "temp": true,
  "_ctime": "2026-09-01T10:00:00.000Z", "_mtime": "2026-09-02T10:00:00.000Z",
  "state": {"lastWatched": "2026-09-02T10:00:00.000Z", "timeWatched": 0, "timeOffset": 600000,
            "overallTimeWatched": 0, "timesWatched": 0, "flaggedWatched": 0, "duration": 2640000,
            "videoId": "tt1:1:2", "watched": null, "noNotif": false},
  "behaviorHints": {}, "gmcaUnknown": 7
})";

int main() {
    // stremio-core's item
    const json series = json::parse(stremioSeries);
    LibraryEntry e = libraryEntry(series);
    CHECK_EQ(e.id, "tt0903747");
    CHECK_EQ(e.type, "series");
    CHECK_EQ(e.name, "Breaking Bad");
    CHECK_EQ(e.poster, "https://images.metahub.space/poster/medium/tt0903747/img");
    CHECK(!e.removed && !e.temp);
    CHECK(e.state.timeOffsetMs == 1500000 && e.state.durationMs == 2820000 && !e.state.flaggedWatched);
    CHECK_EQ(e.state.videoId, "tt0903747:2:5");
    CHECK_EQ(e.state.lastWatched, "2026-10-10T08:30:25.123Z");
    CHECK(continueWatching({e}, 20).size() == 1);
    CHECK(resumePositionMs(e.state, "tt0903747:2:5") == 1500000);
    CHECK(resumePositionMs(e.state, "tt0903747:2:6") == 0);

    const json movie = json::parse(stremioMovie);
    LibraryEntry m = libraryEntry(movie);
    CHECK(m.removed && m.temp && m.state.flaggedWatched);
    CHECK(continueWatching({m}, 20).size() == 1);  // flagged at 75%, not finished: still in the row
    CHECK(resumePositionMs(m.state, "") == 4000000);

    // an older GMCA's videoId
    json legacy = json::parse(legacySeries);
    CHECK_EQ(libraryEntry(legacy).state.videoId, "tt1:1:2");
    CHECK(resumePositionMs(libraryEntry(legacy).state, "tt1:1:2") == 600000);
    // both: Stremio's clients drop keys they don't know whenever they save, so an item that still
    // has `videoId` was last saved by GMCA, and its videoId goes with its timeOffset
    json both = legacy;
    both["state"]["video_id"] = "tt1:1:1";
    CHECK_EQ(libraryEntry(both).state.videoId, "tt1:1:2");
    json none = series;
    none["state"]["video_id"] = nullptr;
    CHECK_EQ(libraryEntry(none).state.videoId, "");
    // partial entries
    CHECK(libraryEntry(json::object()).id.empty());
    CHECK(libraryEntry(json::parse(R"({"_id": "x", "state": null})")).state.timeOffsetMs == 0);

    // before GMCA saves an item: the legacy key moves to video_id, everything else stays
    json moved = legacy;
    normalizeLibraryItem(moved);
    CHECK(!moved["state"].contains("videoId"));
    CHECK_EQ(moved["state"]["video_id"].get<std::string>(), "tt1:1:2");
    CHECK(moved["state"]["timeOffset"] == 600000 && moved["gmcaUnknown"] == 7);
    CHECK(moved["_mtime"] == legacy["_mtime"]);
    json movedBoth = both;
    normalizeLibraryItem(movedBoth);
    CHECK_EQ(movedBoth["state"]["video_id"].get<std::string>(), "tt1:1:2");
    json nullLegacy = series;
    nullLegacy["state"]["videoId"] = nullptr;  // an older GMCA's new entry, never played
    normalizeLibraryItem(nullLegacy);
    CHECK(nullLegacy == series);
    json untouched = series;
    normalizeLibraryItem(untouched);
    CHECK(untouched == series);

    // a position saved while playing
    json st = series["state"];
    applyProgress(st, "tt0903747:2:5", 1600000, 0);  // the same episode, no length from the player
    CHECK(st["timeOffset"] == 1600000 && st["duration"] == 2820000 && st["video_id"] == "tt0903747:2:5");
    applyProgress(st, "tt0903747:2:6", 30000, 0);  // another episode: the old length goes
    CHECK(st["timeOffset"] == 30000 && st["duration"] == 0 && st["video_id"] == "tt0903747:2:6");
    applyProgress(st, "tt0903747:2:6", 60000, 2700000);
    CHECK(st["timeOffset"] == 60000 && st["duration"] == 2700000);
    json legacyState = legacy["state"];
    applyProgress(legacyState, "tt1:1:3", 1000, 2600000);
    CHECK(!legacyState.contains("videoId") && legacyState["video_id"] == "tt1:1:3" &&
          legacyState["duration"] == 2600000);
    json movieState = movie["state"];
    applyProgress(movieState, "", 4100000, 5340000);  // a movie: its video_id untouched
    CHECK(movieState["timeOffset"] == 4100000 && movieState["video_id"] == "tt0013442");

    // "Mark as watched" on an episode
    json adv = series["state"];
    CHECK(applyEpisodeWatched(adv, {EpisodeWatched::Advance, 3}, "tt0903747:2:6"));
    CHECK(adv["video_id"] == "tt0903747:2:6" && adv["timeOffset"] == 1 && adv["duration"] == 0);
    CHECK(adv["watched"] == series["state"]["watched"]);
    json clr = series["state"];
    CHECK(applyEpisodeWatched(clr, {EpisodeWatched::Clear, -1}, ""));
    CHECK(clr["timeOffset"] == 0 && clr["video_id"] == "tt0903747:2:5" && clr["duration"] == 2820000);
    json keep = series["state"];
    CHECK(!applyEpisodeWatched(keep, {EpisodeWatched::Keep, -1}, ""));
    CHECK(keep == series["state"]);

    if (failures == 0) printf("test_library_item: all passed\n");
    return failures ? 1 : 0;
}
