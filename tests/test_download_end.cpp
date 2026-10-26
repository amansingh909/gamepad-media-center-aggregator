// Standalone logic test: how a finished transfer is classified (cancel wins over a throw).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_download_end.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
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
    // a failure with another source left falls back; a cancel or a remove never does
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(true, false), 2) == DownloadNext::Fallback);
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(true, false), 0) == DownloadNext::Failed);
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(true, true), 2) == DownloadNext::Cancelled);
    CHECK(nextAfterTransfer(true, classifyDownloadEnd(true, true), 2) == DownloadNext::Remove);
    CHECK(nextAfterTransfer(false, classifyDownloadEnd(false, false), 2) == DownloadNext::Completed);
    // the playable sources after the chosen one, in the list's order, 3 in all at most
    CHECK((fallbackSources({true, true, true, true}, 0) == std::vector<size_t>{1, 2}));
    CHECK((fallbackSources({true, false, true, false, true}, 0) == std::vector<size_t>{2, 4}));
    CHECK((fallbackSources({true, true, true}, 1) == std::vector<size_t>{2}));
    CHECK(fallbackSources({true, true, true}, 2).empty());
    CHECK(fallbackSources({}, 0).empty());
    // from a detail's medias (the first part key of each, "" when none): the first with a key,
    // then the fallbacks after it
    {
        const SourcePick p = pickSources({"", "a", "", "b", "c", "d"});
        CHECK(p.first == "a");
        CHECK((p.fallbacks == std::vector<std::string>{"b", "c"}));
        CHECK(pickSources({"x"}).fallbacks.empty());
        CHECK(pickSources({"", ""}).first.empty());
        CHECK(pickSources({}).first.empty());
    }
    if (fails) return 1;
    std::printf("test_download_end: all passed\n");
    return 0;
}
