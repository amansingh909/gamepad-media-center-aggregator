#pragma once

/// The player's control map (VideoView's OSD) as plain data: which control the D-pad moves to,
/// how far a seek key jumps and which face button does what. No borealis in here:
/// tests/test_osd_controls.cpp checks it.
///
///     [Back] title             [prev] [-10] [+10] [next] [settings]
///     [play/pause] [=========== progress ===========] time / duration
///     [volume] [Episodes] [Audio] [Subtitles] [Quality] [Lock]
///
/// Left and right move along a row and wrap at its ends. Up and down go to the first shown control
/// of the row above or below; there is nothing above the top row or below the bottom one. On the
/// progress bar left and right seek, so focus stays, and up goes back to play/pause. Hidden
/// controls are skipped: Episodes outside a series, previous/next episode at the ends of a list,
/// Quality for local files, the progress bar for a playlist or with the TV-style OSD off. While
/// the player is locked only Lock is shown.

#include <array>
#include <cstdint>

namespace osd {

enum Control : int {
    // top row
    CLOSE,
    PREV,
    SEEK_BACK,
    SEEK_FWD,
    NEXT,
    SETTINGS,
    // middle row
    TOGGLE,
    SLIDER,
    // bottom row
    VOLUME,
    EPISODES,
    AUDIO,
    SUBTITLES,
    QUALITY,
    LOCK,
    COUNT
};

enum class Dir { UP, DOWN, LEFT, RIGHT };

/// Which controls are on screen.
using Shown = std::array<bool, COUNT>;

inline Shown allShown() {
    Shown s;
    s.fill(true);
    return s;
}

/// First and last control of each row, top to bottom.
struct Row {
    int first, last;
};
inline constexpr Row ROWS[] = {{CLOSE, SETTINGS}, {TOGGLE, SLIDER}, {VOLUME, LOCK}};
inline constexpr int ROW_COUNT = 3;

inline int rowOf(Control c) {
    for (int r = 0; r < ROW_COUNT; r++)
        if (c >= ROWS[r].first && c <= ROWS[r].last) return r;
    return -1;
}

/// Where focus goes from `from` when the D-pad is pressed towards `dir`: `from` itself when
/// there is nowhere to go.
inline Control next(Control from, Dir dir, const Shown& shown) {
    int r = rowOf(from);
    if (r < 0) return from;
    if (dir == Dir::LEFT || dir == Dir::RIGHT) {
        if (from == SLIDER) return SLIDER;
        const Row& row = ROWS[r];
        int n = row.last - row.first + 1;
        int step = dir == Dir::RIGHT ? 1 : -1;
        for (int i = 1; i < n; i++) {
            int c = row.first + ((from - row.first + step * i) % n + n) % n;
            if (shown[c]) return (Control)c;
        }
        return from;
    }
    if (from == SLIDER && dir == Dir::UP && shown[TOGGLE]) return TOGGLE;
    int step = dir == Dir::DOWN ? 1 : -1;
    for (int other = r + step; other >= 0 && other < ROW_COUNT; other += step)
        for (int c = ROWS[other].first; c <= ROWS[other].last; c++)
            if (shown[c]) return (Control)c;
    return from;
}

/// D-pad presses from `start` to each control along the routes of next(); -1 for a control that
/// can't be reached (a hidden one).
inline std::array<int, COUNT> presses(Control start, const Shown& shown) {
    std::array<int, COUNT> dist;
    dist.fill(-1);
    if (!shown[start]) return dist;
    std::array<Control, COUNT> queue{};
    int head = 0, tail = 0;
    dist[start] = 0;
    queue[tail++] = start;
    while (head < tail) {
        Control c = queue[head++];
        for (Dir d : {Dir::UP, Dir::DOWN, Dir::LEFT, Dir::RIGHT}) {
            Control to = next(c, d, shown);
            if (dist[to] >= 0) continue;
            dist[to] = dist[c] + 1;
            queue[tail++] = to;
        }
    }
    return dist;
}

/// Seconds one press of a seek key moves.
inline constexpr int SEEK_STEP = 10;

/// What one more press adds to a seek still waiting to be applied (`pending` seconds, negative
/// backwards): SEEK_STEP, more on a long hold (20 s past 5 min, 60 s past 10 min, a fifteenth
/// of the total past 20 min).
inline int seekStep(int64_t pending) {
    int64_t a = pending < 0 ? -pending : pending;
    if (a < 300) return SEEK_STEP;
    if (a < 600) return 20;
    if (a < 1200) return 60;
    return (int)(a / 15);
}

/// The pending seek after one more press, back or forward.
inline int64_t addSeek(int64_t pending, bool forward) {
    return forward ? pending + seekStep(pending) : pending - seekStep(pending);
}

/// addSeek() kept within the file: from `position` (seconds) no further back than its start, no
/// further on than its `duration`, so a long hold stops there instead of growing without end.
/// A file of unknown length (0 or less) doesn't seek: 0.
inline int64_t addSeek(int64_t pending, bool forward, int64_t position, int64_t duration) {
    if (duration <= 0) return 0;
    int64_t back = position > 0 ? -position : 0;
    int64_t ahead = duration > position ? duration - position : 0;
    int64_t next = addSeek(pending, forward);
    return next < back ? back : (next > ahead ? ahead : next);
}

/// The face buttons of the player's shortcuts, as borealis names them.
enum class Face { X, Y };

/// Square locks the player and Triangle opens the subtitles. Borealis names Square BUTTON_X and
/// Triangle BUTTON_Y, but swaps X and Y along with A and B when it swaps the face buttons (a
/// console with Circle as enter, the app's swap option).
inline Face lockButton(bool swapped) { return swapped ? Face::Y : Face::X; }
inline Face subtitleButton(bool swapped) { return swapped ? Face::X : Face::Y; }

}  // namespace osd
