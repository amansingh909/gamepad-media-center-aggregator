#pragma once

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
    Failed,     // "Download Failed"
};

/// A remove (the entry's "removed" marker) wins over everything: it can land while the
/// transfer starts, before curl runs, or after a transfer that succeeded, during the offline
/// capture; the download must still go.
inline DownloadNext nextAfterTransfer(bool removed, const DownloadEnd& end) {
    if (removed) return DownloadNext::Remove;
    if (end.cancelled) return DownloadNext::Cancelled;
    if (end.success) return DownloadNext::Completed;
    return DownloadNext::Failed;
}
