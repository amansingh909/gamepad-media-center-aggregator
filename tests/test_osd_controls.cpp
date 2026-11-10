// Standalone logic test: the player's control map (view/osd_controls.hpp), where the D-pad moves
// between the OSD's three rows, the seek step and the face buttons of the shortcuts. Runs with
// the other standalone tests: tests/run.sh.

#include <cstdio>
#include <string>
#include "view/osd_controls.hpp"

using namespace osd;

static int failures = 0;

static const char* NAMES[] = {"CLOSE", "PREV", "SEEK_BACK", "SEEK_FWD", "NEXT", "SETTINGS", "TOGGLE", "SLIDER",
    "VOLUME", "EPISODES", "AUDIO", "SUBTITLES", "QUALITY", "LOCK"};
static const char* DIRS[] = {"up", "down", "left", "right"};

#define CHECK(cond)                                         \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FAIL: line %d: %s\n", __LINE__, #cond); \
            ++failures;                                     \
        }                                                   \
    } while (0)

#define CHECK_INT(got, want)                                                       \
    do {                                                                           \
        long long g = (got), w = (want);                                           \
        if (g != w) {                                                              \
            printf("FAIL: line %d: %s = %lld, want %lld\n", __LINE__, #got, g, w); \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

// from -> {up, down, left, right}
static void checkRoutes(const char* config, const Shown& shown, Control from, Control up, Control down,
    Control left, Control right) {
    const Control want[] = {up, down, left, right};
    for (int d = 0; d < 4; d++) {
        Control got = next(from, (Dir)d, shown);
        if (got != want[d]) {
            printf("FAIL: %s: %s %s -> %s, want %s\n", config, NAMES[from], DIRS[d], NAMES[got], NAMES[want[d]]);
            ++failures;
        }
    }
}

static Shown with(std::initializer_list<Control> hidden) {
    Shown s = allShown();
    for (Control c : hidden) s[c] = false;
    return s;
}

int main() {
    // a movie with the TV-style OSD (the default): no previous/next episode, no Episodes
    const Shown movie = with({PREV, NEXT, EPISODES});
    checkRoutes("movie", movie, CLOSE, CLOSE, TOGGLE, SETTINGS, SEEK_BACK);
    checkRoutes("movie", movie, SEEK_BACK, SEEK_BACK, TOGGLE, CLOSE, SEEK_FWD);
    checkRoutes("movie", movie, SEEK_FWD, SEEK_FWD, TOGGLE, SEEK_BACK, SETTINGS);
    checkRoutes("movie", movie, SETTINGS, SETTINGS, TOGGLE, SEEK_FWD, CLOSE);
    checkRoutes("movie", movie, TOGGLE, CLOSE, VOLUME, SLIDER, SLIDER);
    // Plan 3 Task 11: up from the progress bar is play/pause, down the bottom row; left and
    // right seek there, so focus stays
    checkRoutes("movie", movie, SLIDER, TOGGLE, VOLUME, SLIDER, SLIDER);
    checkRoutes("movie", movie, VOLUME, TOGGLE, VOLUME, LOCK, AUDIO);
    checkRoutes("movie", movie, AUDIO, TOGGLE, AUDIO, VOLUME, SUBTITLES);
    checkRoutes("movie", movie, SUBTITLES, TOGGLE, SUBTITLES, AUDIO, QUALITY);
    // the owner's report: right from Quality goes to Lock
    checkRoutes("movie", movie, QUALITY, TOGGLE, QUALITY, SUBTITLES, LOCK);
    checkRoutes("movie", movie, LOCK, TOGGLE, LOCK, QUALITY, VOLUME);

    // presses from play/pause, where focus lands when the controls show
    {
        auto p = presses(TOGGLE, movie);
        CHECK_INT(p[TOGGLE], 0);
        CHECK_INT(p[CLOSE], 1);
        CHECK_INT(p[SLIDER], 1);
        CHECK_INT(p[VOLUME], 1);
        CHECK_INT(p[SEEK_BACK], 2);
        CHECK_INT(p[SETTINGS], 2);
        CHECK_INT(p[AUDIO], 2);
        CHECK_INT(p[LOCK], 2);
        CHECK_INT(p[SEEK_FWD], 3);
        CHECK_INT(p[SUBTITLES], 3);
        CHECK_INT(p[QUALITY], 3);
        CHECK_INT(p[PREV], -1);
        CHECK_INT(p[NEXT], -1);
        CHECK_INT(p[EPISODES], -1);
    }

    // a series, an episode in the middle of the list: everything shown
    const Shown series = allShown();
    checkRoutes("series", series, CLOSE, CLOSE, TOGGLE, SETTINGS, PREV);
    checkRoutes("series", series, PREV, PREV, TOGGLE, CLOSE, SEEK_BACK);
    checkRoutes("series", series, SEEK_FWD, SEEK_FWD, TOGGLE, SEEK_BACK, NEXT);
    checkRoutes("series", series, NEXT, NEXT, TOGGLE, SEEK_FWD, SETTINGS);
    checkRoutes("series", series, VOLUME, TOGGLE, VOLUME, LOCK, EPISODES);
    checkRoutes("series", series, EPISODES, TOGGLE, EPISODES, VOLUME, AUDIO);
    checkRoutes("series", series, AUDIO, TOGGLE, AUDIO, EPISODES, SUBTITLES);

    // the first episode: no previous one
    const Shown first = with({PREV});
    checkRoutes("first episode", first, CLOSE, CLOSE, TOGGLE, SETTINGS, SEEK_BACK);
    checkRoutes("first episode", first, SEEK_BACK, SEEK_BACK, TOGGLE, CLOSE, SEEK_FWD);

    // a remote playlist: no progress bar, no quality
    const Shown playlist = with({PREV, NEXT, EPISODES, SLIDER, QUALITY});
    checkRoutes("playlist", playlist, TOGGLE, CLOSE, VOLUME, TOGGLE, TOGGLE);
    checkRoutes("playlist", playlist, SUBTITLES, TOGGLE, SUBTITLES, AUDIO, LOCK);
    checkRoutes("playlist", playlist, LOCK, TOGGLE, LOCK, SUBTITLES, VOLUME);

    // locked: only the lock button, every way leads back to it
    Shown locked{};
    locked[LOCK] = true;
    checkRoutes("locked", locked, LOCK, LOCK, LOCK, LOCK, LOCK);

    // every combination of the controls that come and go: a route never lands on a hidden
    // control, left and right never dead-end in a row of two or more, every shown control can
    // be reached from play/pause and Lock is at most two presses away
    const Control optional[] = {PREV, NEXT, EPISODES, SLIDER, QUALITY};
    for (int mask = 0; mask < 32; mask++) {
        Shown s = allShown();
        for (int i = 0; i < 5; i++)
            if (mask & (1 << i)) s[optional[i]] = false;
        auto p = presses(TOGGLE, s);
        for (int c = 0; c < COUNT; c++) {
            if (!s[c]) {
                CHECK_INT(p[c], -1);
                continue;
            }
            if (p[c] < 0) {
                printf("FAIL: mask %d: %s unreachable from play/pause\n", mask, NAMES[c]);
                ++failures;
            }
            int shownInRow = 0;
            for (int o = ROWS[rowOf((Control)c)].first; o <= ROWS[rowOf((Control)c)].last; o++) shownInRow += s[o];
            for (int d = 0; d < 4; d++) {
                Control to = next((Control)c, (Dir)d, s);
                if (!s[to]) {
                    printf("FAIL: mask %d: %s %s -> hidden %s\n", mask, NAMES[c], DIRS[d], NAMES[to]);
                    ++failures;
                }
                bool sideways = d == (int)Dir::LEFT || d == (int)Dir::RIGHT;
                if (sideways && c != SLIDER && shownInRow > 1 && to == c) {
                    printf("FAIL: mask %d: %s %s dead-ends\n", mask, NAMES[c], DIRS[d]);
                    ++failures;
                }
            }
        }
        if (p[LOCK] < 0 || p[LOCK] > 2) {
            printf("FAIL: mask %d: Lock is %d presses from play/pause\n", mask, p[LOCK]);
            ++failures;
        }
    }

    // the seek step: 10 s a press, more on a long hold
    CHECK_INT(seekStep(0), 10);
    CHECK_INT(seekStep(290), 10);
    CHECK_INT(seekStep(-299), 10);
    CHECK_INT(seekStep(300), 20);
    CHECK_INT(seekStep(-650), 60);
    CHECK_INT(seekStep(1500), 100);
    CHECK_INT(addSeek(0, true), 10);
    CHECK_INT(addSeek(0, false), -10);
    CHECK_INT(addSeek(10, true), 20);
    CHECK_INT(addSeek(-10, false), -20);
    CHECK_INT(addSeek(30, false), 20);
    {
        // thirty presses reach five minutes, then the step grows
        int64_t pending = 0;
        for (int i = 0; i < 30; i++) pending = addSeek(pending, true);
        CHECK_INT(pending, 300);
        CHECK_INT(addSeek(pending, true), 320);
    }

    // a held seek stays within the file: no further back than its start, no further on than its
    // end, so it can't grow past what an int holds either
    CHECK_INT(addSeek(0, true, 100, 1200), 10);
    CHECK_INT(addSeek(0, false, 5, 1200), -5);
    CHECK_INT(addSeek(0, true, 1195, 1200), 5);
    CHECK_INT(addSeek(-5, false, 5, 1200), -5);
    CHECK_INT(addSeek(20, false, 100, 1200), 10);
    {
        int64_t pending = 0;
        bool sane = true;
        for (int i = 0; i < 100000; i++) {
            pending = addSeek(pending, true, 0, 7200);
            if (pending < 0 || pending > 7200) sane = false;
        }
        CHECK(sane);
        CHECK_INT(pending, 7200);
        pending = 0;
        for (int i = 0; i < 100000; i++) pending = addSeek(pending, false, 3600, 7200);
        CHECK_INT(pending, -3600);
    }
    // a file of unknown length: no seek
    CHECK_INT(addSeek(0, true, 100, 0), 0);
    CHECK_INT(addSeek(0, false, 100, -1), 0);
    // mpv's clock a little past the length: forward stays put, back still works
    CHECK_INT(addSeek(0, true, 1201, 1200), 0);
    CHECK_INT(addSeek(0, false, 1201, 1200), -10);

    // Square locks and Triangle opens the subtitles: borealis calls them X and Y, and swaps them
    // along with A and B (a console with Circle as enter, the app's swap option)
    CHECK(lockButton(false) == Face::X);
    CHECK(subtitleButton(false) == Face::Y);
    CHECK(lockButton(true) == Face::Y);
    CHECK(subtitleButton(true) == Face::X);

    if (failures == 0) printf("test_osd_controls: all passed\n");
    return failures ? 1 : 0;
}
