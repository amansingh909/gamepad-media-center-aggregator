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
