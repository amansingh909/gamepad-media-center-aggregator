// Standalone logic test: what an HDR file gets on the PS4 (view/hdr_options.hpp).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_hdr_options.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>
#include "view/hdr_options.hpp"

static int failures = 0;
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    using namespace hdr;
    CHECK_EQ(isHdrTransfer("pq") ? "y" : "n", "y");
    CHECK_EQ(isHdrTransfer("hlg") ? "y" : "n", "y");
    for (const char* name : {"bt.1886", "srgb", "gamma2.2", "linear", "", "PQ"})
        CHECK_EQ(isHdrTransfer(name) ? "y" : "n", "n");

    CHECK_EQ(retagFilter(), "@hdr:format=gamma=bt.1886:primaries=bt.709:sig-peak=1");
    CHECK_EQ(filterLabel(), "@hdr");

    for (int t : {Automatic, RetagOnly, RetagLow, RetagHigh}) CHECK_EQ(usesRetag(t) ? "y" : "n", "y");
    for (int t : {MpvDefaults, FourKTest}) CHECK_EQ(usesRetag(t) ? "y" : "n", "n");

    CHECK_EQ(filterChain(0, true), retagFilter());
    CHECK_EQ(filterChain(1, true), retagFilter() + ",hflip");
    CHECK_EQ(filterChain(2, true), retagFilter() + ",vflip");
    CHECK_EQ(filterChain(0, false), "");
    CHECK_EQ(filterChain(1, false), "hflip");
    CHECK_EQ(filterChain(2, false), "vflip");

    // not HDR: nothing, whatever the choice
    for (int t = 0; t < testCount; ++t) CHECK_EQ(describe(planFor(t, "bt.1886")), "none");

    CHECK_EQ(describe(planFor(Automatic, "pq")), "re-tag, saturation +15, contrast +10");
    CHECK_EQ(describe(planFor(Automatic, "hlg")), "none");  // HLG: re-tag only, no compensation
    for (int t = 0; t < testCount; ++t) CHECK_EQ(describe(planFor(t, "hlg")), "none");
    CHECK_EQ(describe(planFor(MpvDefaults, "pq")), "none");
    CHECK_EQ(describe(planFor(RetagOnly, "pq")), "re-tag, saturation +0, contrast +0");
    CHECK_EQ(describe(planFor(RetagLow, "pq")), "re-tag, saturation +8, contrast +5");
    CHECK_EQ(describe(planFor(RetagHigh, "pq")), "re-tag, saturation +30, contrast +25");
    CHECK_EQ(describe(planFor(FourKTest, "pq")), "none");

    // low < automatic < high, so the console comparison is an ordered scale
    CHECK_EQ(planFor(RetagLow, "pq").saturation < planFor(Automatic, "pq").saturation &&
                     planFor(Automatic, "pq").saturation < planFor(RetagHigh, "pq").saturation &&
                     planFor(RetagLow, "pq").contrast < planFor(Automatic, "pq").contrast &&
                     planFor(Automatic, "pq").contrast < planFor(RetagHigh, "pq").contrast
                 ? "ok"
                 : "bad",
        "ok");

    CHECK_EQ(describe(startOptions(FourKTest)), "vd-lavc-skiploopfilter=all vd-lavc-fast=yes framedrop=decoder+vo");
    for (int t : {Automatic, MpvDefaults, RetagOnly, RetagLow, RetagHigh}) CHECK_EQ(describe(startOptions(t)), "none");

    CHECK_EQ(std::to_string(clampEqualizer(95 + 30)), "100");
    CHECK_EQ(std::to_string(clampEqualizer(-95 - 30)), "-100");
    CHECK_EQ(std::to_string(clampEqualizer(5 + 15)), "20");
    CHECK_EQ(describe(std::vector<Option>{}), "none");
    CHECK_EQ(testLabels().size() == size_t(testCount) ? "ok" : "bad", "ok");

    if (failures) return 1;
    printf("hdr_options: all checks passed\n");
    return 0;
}
