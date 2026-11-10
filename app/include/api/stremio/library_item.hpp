// A Stremio account libraryItem (the datastore's JSON) as GMCA reads and changes it: what
// Continue Watching reads (resume.hpp) and the state changes GMCA saves. The keys are
// stremio-core's (src/types/library/library_item.rs): camelCase, except the state's `video_id`.
// Header-only, nlohmann and resume.hpp only (tests/test_library_item.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
#include "api/media/types.hpp"
#include "api/stremio/resume.hpp"

namespace stremio {

/// The episode a series' state was left at: `video_id`. Older GMCA builds wrote `videoId`
/// instead; Stremio's clients drop keys they don't know whenever they save an item, so an item
/// that still has `videoId` was last saved by GMCA, and that one goes with its timeOffset.
inline std::string stateVideoId(const nlohmann::json& state) {
    const std::string legacy = media::jstr(state, "videoId");
    return legacy.empty() ? media::jstr(state, "video_id") : legacy;
}

/// Sets the state's episode, under stremio-core's key only.
inline void setStateVideoId(nlohmann::json& state, const std::string& videoId) {
    state["video_id"] = videoId;
    state.erase("videoId");
}

/// One libraryItem's `state` as the Continue Watching rules read it.
inline LibraryState libraryState(const nlohmann::json& state) {
    LibraryState s;
    if (!state.is_object()) return s;
    s.timeOffsetMs = media::jint(state, "timeOffset");
    s.durationMs = media::jint(state, "duration");
    s.flaggedWatched = media::jint(state, "flaggedWatched") > 0;
    s.videoId = stateVideoId(state);
    s.lastWatched = media::jstr(state, "lastWatched");
    return s;
}

/// A datastore libraryItem as the Continue Watching rules read it.
inline LibraryEntry libraryEntry(const nlohmann::json& item) {
    LibraryEntry e;
    if (!item.is_object()) return e;
    e.id = media::jstr(item, "_id");
    e.type = media::jstr(item, "type");
    e.name = media::jstr(item, "name");
    e.poster = media::jstr(item, "poster");
    e.removed = media::jbool(item, "removed");
    e.temp = media::jbool(item, "temp");
    auto st = item.find("state");
    if (st != item.end()) e.state = libraryState(*st);
    return e;
}

/// Before GMCA saves an item: an older build's `state.videoId` moves to `state.video_id`.
/// Every other key stays as it was.
inline void normalizeLibraryItem(nlohmann::json& item) {
    auto st = item.find("state");
    if (st == item.end() || !st->is_object() || !st->contains("videoId")) return;
    const std::string legacy = media::jstr(*st, "videoId");
    if (legacy.empty())
        st->erase("videoId");
    else
        setStateVideoId(*st, legacy);
}

/// A position saved while (or after) playing: the episode's id when it is one, and its length
/// when the player knows it. Another episode than the saved one starts with no length, so the
/// last one's is never read as its own.
inline void applyProgress(nlohmann::json& state, const std::string& videoId, int64_t posMs, int64_t durMs) {
    if (!videoId.empty()) {
        if (stateVideoId(state) != videoId) state["duration"] = 0;
        setStateVideoId(state, videoId);
    }
    state["timeOffset"] = posMs;
    if (durMs > 0) state["duration"] = durMs;
}

/// "Mark as watched" on an episode, as episodeWatched() decided; `nextVideoId` is the next
/// episode's id for Advance. False when nothing changes (Keep): the item needn't be saved.
inline bool applyEpisodeWatched(nlohmann::json& state, const EpisodeWatched& w, const std::string& nextVideoId) {
    switch (w.kind) {
        case EpisodeWatched::Advance:
            setStateVideoId(state, nextVideoId);
            state["timeOffset"] = 1;  // stremio-core's advance_to_video: in the row, from the start
            state["duration"] = 0;
            return true;
        case EpisodeWatched::Clear:
            state["timeOffset"] = 0;
            return true;
        case EpisodeWatched::Keep:
            break;
    }
    return false;
}

}  // namespace stremio
