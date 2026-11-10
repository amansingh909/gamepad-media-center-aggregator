// Standalone logic test: the recent download speed and when a torrent row says "May buffer".
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_buffer.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <torrent/buffer_hint.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

int main() {
    using namespace torrent;
    const int64_t KiB = 1024, MiB = int64_t(1) << 20, GiB = int64_t(1) << 30;
    // a slow average of the samples that moved data
    CHECK(nextRecentRate(0, 0) == 0);
    CHECK(nextRecentRate(0, 1000 * KiB) == 1000 * KiB);
    CHECK(nextRecentRate(1000 * KiB, 0) == 1000 * KiB);
    CHECK(nextRecentRate(1000 * KiB, 2000 * KiB) == 1100 * KiB);
    // samples under 64 KiB/s (a full window, a throttled grace torrent) are ignored
    CHECK(minRateSample == 64 * KiB);
    CHECK(nextRecentRate(0, 64 * KiB - 1) == 0);
    CHECK(nextRecentRate(0, 64 * KiB) == 64 * KiB);
    CHECK(nextRecentRate(1000 * KiB, 64 * KiB - 1) == 1000 * KiB);
    CHECK(nextRecentRate(1000 * KiB, 10) == 1000 * KiB);
    // 4 GiB over 2 hours is about 0.57 MiB/s: fine at 1 MiB/s
    CHECK(!mayBuffer(4 * GiB, 120 * 60000, MiB));
    // 10 GiB over 100 minutes is about 1.7 MiB/s: more than a quarter above 1 MiB/s
    CHECK(mayBuffer(10 * GiB, 100 * 60000, MiB));
    // just above the speed is not "clearly above"
    CHECK(!mayBuffer(int64_t(1.1 * 6000) * MiB, 100 * 60000, MiB));
    // anything unknown: no hint
    CHECK(!mayBuffer(-1, 100 * 60000, MiB));
    CHECK(!mayBuffer(10 * GiB, 0, MiB));
    CHECK(!mayBuffer(10 * GiB, 100 * 60000, 0));
    if (failures == 0) printf("test_torrent_buffer: all passed\n");
    return failures ? 1 : 0;
}
