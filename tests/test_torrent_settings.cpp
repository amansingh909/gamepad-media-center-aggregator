// Standalone logic test: the torrent upload limit's Settings choices and stored values.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_settings.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <vector>
#include <torrent/torrent_engine.hpp>
#include <torrent/upload_limits.hpp>

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

    std::vector<std::string> labels = uploadLimitLabels();
    CHECK(labels == std::vector<std::string>({"256 KB/s", "512 KB/s", "1 MB/s", "2 MB/s"}));
    CHECK(uploadLimitOptions[0].bytesPerSec == 262144 && uploadLimitOptions[3].bytesPerSec == 2097152);

    // 1 MB/s by default, the engine's own default
    CHECK(defaultUploadLimitIndex == 2 && defaultUploadLimit == 1048576);
    CHECK(defaultUploadLimit == EngineSettings{}.uploadLimit);

    // each choice maps back to itself
    for (int i = 0; i < uploadLimitCount; ++i) {
        CHECK(uploadLimitIndex(uploadLimitOptions[i].bytesPerSec) == i);
        CHECK(uploadLimitValue(uploadLimitOptions[i].bytesPerSec) == uploadLimitOptions[i].bytesPerSec);
    }

    // anything else stored is the default
    CHECK(uploadLimitIndex(0) == 2 && uploadLimitIndex(-1) == 2 && uploadLimitIndex(1000) == 2);
    CHECK(uploadLimitIndex(int64_t(1) << 40) == 2);
    CHECK(uploadLimitValue(12345) == 1048576);

    if (failures == 0) printf("test_torrent_settings: all passed\n");
    return failures ? 1 : 0;
}
