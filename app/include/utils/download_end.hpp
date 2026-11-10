#pragma once

#include <cstddef>
#include <string>
#include <vector>

/*
    How a finished transfer is classified. A user's cancel aborts curl through the
    progress callback, so the transfer throws; the cancel flag must be read on that
    path too, or the cancel (and removing an active download) is reported as a failure.
    STL only, so tests/test_download_end.cpp can exercise it.
*/

struct DownloadEnd {
    bool cancelled;
    bool success;
};

inline DownloadEnd classifyDownloadEnd(bool threw, bool cancelFlag) {
    return DownloadEnd{cancelFlag, !threw && !cancelFlag};
}

/// What the queue does with a download once its transfer has ended (or never started).
enum class DownloadNext {
    Remove,     // removed meanwhile: its entry and its folder go
    Cancelled,  // cancelled: kept, with its partial file, for a retry
    Completed,
    Fallback,   // failed, and another source is left: that one, from the start
    Failed,     // "Download Failed"
};

/// A remove (the entry's "removed" marker) wins over everything: it can land while the
/// transfer starts, before curl runs, or after a transfer that succeeded, during the offline
/// capture; the download must still go.
/// `fallbacksLeft`: the sources the download still has after this one; a failure falls back
/// to the next while there is one. A cancel or a remove never falls back.
inline DownloadNext nextAfterTransfer(bool removed, const DownloadEnd& end, size_t fallbacksLeft = 0) {
    if (removed) return DownloadNext::Remove;
    if (end.cancelled) return DownloadNext::Cancelled;
    if (end.success) return DownloadNext::Completed;
    return fallbacksLeft > 0 ? DownloadNext::Fallback : DownloadNext::Failed;
}

/// At most this many sources are tried for one download: the chosen one and two more.
constexpr size_t maxDownloadSources = 3;

/// The sources a download from a title's list falls back to: the playable rows after the
/// chosen one (`playable[i]` for row i), in the list's order, best first.
inline std::vector<size_t> fallbackSources(const std::vector<bool>& playable, size_t chosen) {
    std::vector<size_t> out;
    for (size_t i = chosen + 1; i < playable.size() && out.size() + 1 < maxDownloadSources; ++i)
        if (playable[i]) out.push_back(i);
    return out;
}

/// The source a download takes from a detail's medias (`mediaKeys[i]`: media i's first
/// non-empty part key, or "" when it has none), and the keys it falls back to.
struct SourcePick {
    std::string first;
    std::vector<std::string> fallbacks;
};

inline SourcePick pickSources(const std::vector<std::string>& mediaKeys) {
    SourcePick pick;
    std::vector<bool> playable;
    size_t chosen = 0;
    for (size_t i = 0; i < mediaKeys.size(); ++i) {
        playable.push_back(!mediaKeys[i].empty());
        if (pick.first.empty() && !mediaKeys[i].empty()) {
            pick.first = mediaKeys[i];
            chosen = i;
        }
    }
    if (!pick.first.empty())
        for (size_t f : fallbackSources(playable, chosen)) pick.fallbacks.push_back(mediaKeys[f]);
    return pick;
}
