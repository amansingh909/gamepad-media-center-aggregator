#pragma once

#include <algorithm>
#include <cstdint>

/*
    Where a file that mpv ended had got to. mpv ends a file with reason EOF when it played to
    its end, and also when the connection drops mid-file: a Wi-Fi drop, a dying transcoder, a
    torrent the engine gave up on (the server can only close the connection, and mpv takes that
    for the end once it has played what it had). Every comparison is between mpv's own clock and
    mpv's own length, in seconds. STL only, so tests/test_playback_end.cpp can exercise it.
*/

namespace playback {

/// How short of a file's duration an end of the file must come to count as a stream that broke
/// off rather than one that played to its end.
constexpr double endSlackSecs = 5;

/// mpv ended a file at `positionSecs` of `durationSecs`: true if it ended more than
/// endSlackSecs early. A duration mpv doesn't know says nothing.
inline bool endedEarly(int64_t durationSecs, double positionSecs) {
    return durationSecs > 0 && positionSecs + endSlackSecs < double(durationSecs);
}

/// mpv's clock reached mpv's length (within endSlackSecs): the file played to its end. False
/// when the length isn't known.
inline bool reachedEnd(int64_t durationSecs, double positionSecs) {
    return durationSecs > 0 && !endedEarly(durationSecs, positionSecs);
}

/// The position the stop report of an ended file carries: the length when the file played to
/// its end (the item's length `lengthMs` when longer, as a server's length can differ a little
/// from the file's), else `lastMs`, where playback really stopped.
inline int64_t endPositionMs(int64_t lastMs, int64_t lengthMs, int64_t mpvDurationSecs, double mpvPositionSecs) {
    return reachedEnd(mpvDurationSecs, mpvPositionSecs) ? std::max(lastMs, lengthMs) : lastMs;
}

}  // namespace playback
