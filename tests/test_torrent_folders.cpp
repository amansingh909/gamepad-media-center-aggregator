// Standalone logic test: what the torrent engine may delete in its temp folder, and
// which temp folders it accepts.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_folders.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include <torrent/temp_folder.hpp>

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
    const std::string hash = "01c137287d6f0ed05a56742dae794f632c79ff3d";

    // the engine's own save folders: <info hash>-<n>
    CHECK(isEngineFolderName(hash + "-1"));
    CHECK(isEngineFolderName(hash + "-12345"));
    CHECK(isEngineFolderName("0000000000000000000000000000000000000002-7"));

    // anything else stays
    CHECK(!isEngineFolderName(""));
    CHECK(!isEngineFolderName("."));
    CHECK(!isEngineFolderName(".."));
    CHECK(!isEngineFolderName("Movies"));
    CHECK(!isEngineFolderName("config.json"));
    CHECK(!isEngineFolderName(hash));                // no counter
    CHECK(!isEngineFolderName(hash + "-"));          // empty counter
    CHECK(!isEngineFolderName(hash + "-1a"));        // counter not all digits
    CHECK(!isEngineFolderName(hash + "-1 "));
    CHECK(!isEngineFolderName(hash + "-1.parts"));
    CHECK(!isEngineFolderName(hash + "_1"));
    CHECK(!isEngineFolderName(hash + "--1"));
    CHECK(!isEngineFolderName(hash.substr(1) + "-1"));  // 39 hex characters
    CHECK(!isEngineFolderName(hash + "0-1"));           // 41
    CHECK(!isEngineFolderName("01C137287D6F0ED05A56742DAE794F632C79FF3D-1"));  // the engine writes lowercase
    CHECK(!isEngineFolderName("g1c137287d6f0ed05a56742dae794f632c79ff3d-1"));  // not hex
    CHECK(!isEngineFolderName("-1"));

    // the engine only works in an absolute folder
    CHECK(isUsableTempDir("/tmp/torrent-serve"));
    CHECK(isUsableTempDir("/data/GMCA/torrents"));
    CHECK(!isUsableTempDir(""));
    CHECK(!isUsableTempDir("torrents"));
    CHECK(!isUsableTempDir("./torrents"));
    CHECK(!isUsableTempDir("../torrents"));

    if (failures == 0) printf("test_torrent_folders: all passed\n");
    return failures ? 1 : 0;
}
