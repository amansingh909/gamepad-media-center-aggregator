// Standalone logic test: what the app shows about torrents (failure messages; the loading
// screen's stages, progress and pulse).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_ui.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <torrent/ui_model.hpp>

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

int main() {
    using namespace torrent;

    // why torrent rows are grey
    CHECK_EQ(availabilityKey(Availability::Off), "main/torrent/error/off");
    CHECK_EQ(availabilityKey(Availability::NotStarted), "main/torrent/error/start");
    CHECK_EQ(availabilityKey(Availability::Ready), "main/torrent/error/unplayable");
    CHECK_EQ(availabilityKey(Availability::Stopped), "main/torrent/error/unplayable");

    if (failures == 0) printf("test_torrent_ui: all passed\n");
    return failures ? 1 : 0;
}
