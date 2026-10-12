// Standalone logic test: how a finished transfer is classified (cancel wins over a throw).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_download_end.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <utils/download_end.hpp>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    // finished normally
    CHECK(classifyDownloadEnd(false, false).success);
    CHECK(!classifyDownloadEnd(false, false).cancelled);
    // cancelled without a throw (the transfer finished as the flag was set)
    CHECK(classifyDownloadEnd(false, true).cancelled);
    CHECK(!classifyDownloadEnd(false, true).success);
    // cancel aborts curl through the progress callback, so the transfer throws
    CHECK(classifyDownloadEnd(true, true).cancelled);
    CHECK(!classifyDownloadEnd(true, true).success);
    // a genuine failure stays a failure
    CHECK(!classifyDownloadEnd(true, false).cancelled);
    CHECK(!classifyDownloadEnd(true, false).success);
    // a remove wins over whatever ended the transfer
    CHECK(nextAfterTransfer(true, classifyDownloadEnd(false, false)) == DownloadNext::Remove);  // in the capture
    CHECK(nextAfterTransfer(true, DownloadEnd{true, false}) == DownloadNext::Remove);           // at startup
    CHECK(nextAfterTransfer(true, classifyDownloadEnd(true, true)) == DownloadNext::Remove);    // mid-transfer
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(true, true)) == DownloadNext::Cancelled);
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(false, false)) == DownloadNext::Completed);
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(true, false)) == DownloadNext::Failed);
    if (fails) return 1;
    std::printf("test_download_end: all passed\n");
    return 0;
}
