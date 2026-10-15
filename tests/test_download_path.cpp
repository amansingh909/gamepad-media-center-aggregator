// Standalone logic test: which item ids may name a folder under the downloads folder.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_download_path.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <utils/download_path.hpp>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    CHECK(!isSafeDownloadId(""));
    CHECK(!isSafeDownloadId("."));
    CHECK(!isSafeDownloadId(".."));
    CHECK(!isSafeDownloadId("a/b"));
    CHECK(!isSafeDownloadId("/etc"));
    CHECK(!isSafeDownloadId("..\\x"));
    CHECK(!isSafeDownloadId("a\\b"));
    CHECK(isSafeDownloadId("12345"));
    CHECK(isSafeDownloadId("tt0111161:1:2"));
    CHECK(isSafeDownloadId("a..b"));
    if (fails) return 1;
    std::printf("test_download_path: all passed\n");
    return 0;
}
