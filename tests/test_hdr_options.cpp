// Standalone logic test: which mpv options an HDR file gets on the PS4 (view/hdr_options.hpp).
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
    CHECK_EQ(isHdrTransfer("bt.1886") ? "y" : "n", "n");
    CHECK_EQ(isHdrTransfer("srgb") ? "y" : "n", "n");
    CHECK_EQ(isHdrTransfer("") ? "y" : "n", "n");
    CHECK_EQ(isHdrTransfer("PQ") ? "y" : "n", "n");  // mpv prints lower case

    // one nit above the video's peak, so it is never above the target
    CHECK_EQ(std::to_string(targetPeakNits(3.472906)), "706");  // 705 nits, one more
    CHECK_EQ(std::to_string(targetPeakNits(4.926108)), "1001");  // 1000 nits, one more
    CHECK_EQ(std::to_string(targetPeakNits(0)), "10000");        // unknown
    CHECK_EQ(std::to_string(targetPeakNits(-1)), "10000");
    CHECK_EQ(std::to_string(targetPeakNits(49.261084)), "10000");  // clamped to mpv's range
    CHECK_EQ(std::to_string(targetPeakNits(0.01)), "10");
    for (double p : {1.5, 3.4729, 4.9261, 12.0, 20.0})
        CHECK_EQ(targetPeakNits(p) / refWhiteNits > p ? "ok" : "bad", "ok");

    // not HDR: nothing, whatever the choice
    for (int t = 0; t < testCount; ++t) CHECK_EQ(describe(hdrOptions(t, 0, "bt.1886", "bt.709", 1.0)), "none");

    CHECK_EQ(describe(hdrOptions(Automatic, 0, "pq", "bt.2020", 3.472906)),
        "target-peak=706 gamut-mapping-mode=clip tone-mapping=clip hdr-compute-peak=no");
    CHECK_EQ(describe(hdrOptions(Automatic, 1, "pq", "bt.2020", 3.472906)),
        "target-trc=pq target-prim=bt.2020 target-peak=706");
    CHECK_EQ(describe(hdrOptions(Automatic, 1, "hlg", "", 0)),
        "target-trc=hlg target-prim=bt.2020 target-peak=10000");
    CHECK_EQ(describe(hdrOptions(Automatic, 1, "pq", "bt.709", 3.0)),
        "target-trc=pq target-prim=bt.709 target-peak=610");
    CHECK_EQ(describe(hdrOptions(MpvDefaults, 0, "pq", "bt.2020", 3.0)), "none");
    CHECK_EQ(describe(hdrOptions(ToneMappingClip, 0, "pq", "bt.2020", 3.0)), "tone-mapping=clip");
    CHECK_EQ(describe(hdrOptions(GamutClip, 0, "pq", "bt.2020", 3.0)), "gamut-mapping-mode=clip");
    CHECK_EQ(describe(hdrOptions(ToneAndGamutClip, 0, "hlg", "bt.2020", 3.0)),
        "tone-mapping=clip gamut-mapping-mode=clip");
    CHECK_EQ(describe(hdrOptions(Passthrough, 0, "pq", "bt.2020", 3.0)),
        "target-trc=pq target-prim=bt.2020 target-peak=610");
    CHECK_EQ(describe(hdrOptions(DumbMode, 0, "pq", "bt.2020", 3.0)), "none");
    CHECK_EQ(describe(hdrOptions(FourKTest, 0, "pq", "bt.2020", 3.0)), "none");

    CHECK_EQ(describe(startOptions(DumbMode)), "gpu-dumb-mode=yes");
    CHECK_EQ(describe(startOptions(FourKTest)),
        "vd-lavc-skiploopfilter=all vd-lavc-fast=yes framedrop=decoder+vo");
    for (int t : {Automatic, MpvDefaults, ToneMappingClip, GamutClip, ToneAndGamutClip, Passthrough})
        CHECK_EQ(describe(startOptions(t)), "none");

    // every option an HDR file can get is reset for the next file
    const auto reset = resetOptions();
    for (int t = 0; t < testCount; ++t)
        for (int level = 0; level < autoLevels; ++level)
            for (const auto& o : hdrOptions(t, level, "pq", "bt.2020", 3.0)) {
                bool found = false;
                for (const auto& r : reset) found = found || r.first == o.first;
                CHECK_EQ(found ? "reset" : "NOT reset: " + o.first, "reset");
            }
    CHECK_EQ(describe({}), "none");

    CHECK_EQ(testLabels().size() == size_t(testCount) ? "ok" : "bad", "ok");
    CHECK_EQ(isShaderLinkFailure("shader link log (status=0):") ? "y" : "n", "y");
    CHECK_EQ(isShaderLinkFailure("fragment shader source:") ? "y" : "n", "n");

    if (failures) return 1;
    printf("hdr_options: all checks passed\n");
    return 0;
}
