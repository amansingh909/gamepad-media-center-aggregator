// Standalone logic test: where a file that mpv ended had got to (played to its end, or a
// connection that dropped mid-file), and the position its stop report carries.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_playback_end.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <utils/playback_end.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

using namespace playback;

int main() {
    // ended early: more than endSlackSecs short of mpv's own length
    CHECK(endedEarly(888, 400.2));
    CHECK(endedEarly(888, 882.9));
    CHECK(!endedEarly(888, 887.96));  // it played to its end
    CHECK(!endedEarly(888, 884));     // within a few seconds of it
    CHECK(!endedEarly(0, 12));        // no length known: nothing to say
    CHECK(!endedEarly(-1, 12));

    // reached the end: only with a length mpv knows
    CHECK(reachedEnd(888, 887.96));
    CHECK(reachedEnd(888, 884));
    CHECK(!reachedEnd(888, 400.2));
    CHECK(!reachedEnd(0, 12));  // a stream with no length: no claim it was watched to the end

    // the stop report's position
    const int64_t movieMs = 89 * 60000;  // the item's length (a Stremio movie's meta runtime)
    // played to its end, the item's length a little longer than the file's: the item's length
    CHECK(endPositionMs(5338000, movieMs, 5339, 5338.9) == movieMs);
    // played to its end, the file a little longer than the item: the last position
    CHECK(endPositionMs(5400000, movieMs, 5401, 5400.5) == 5400000);
    // the connection dropped at 40% (Wi-Fi, a dying transcoder): where it stopped
    CHECK(endPositionMs(2136000, movieMs, 5340, 2136.4) == 2136000);
    // no length from mpv: where it stopped, whatever the item's length
    CHECK(endPositionMs(2136000, movieMs, 0, 2136.4) == 2136000);
    // no item length (a Stremio episode), played to its end: mpv's clock
    CHECK(endPositionMs(2640000, 0, 2641, 2640.9) == 2640000);

    if (failures == 0) printf("test_playback_end: all passed\n");
    return failures ? 1 : 0;
}
