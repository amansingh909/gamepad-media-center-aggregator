// Standalone logic test — label of a movie version in the Version selector.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include \
//       -Ilibrary/borealis/library/include/borealis/extern \
//       tests/test_version_label.cpp -o /tmp/t && /tmp/t
//
// Jellyfin/Emby leave videoResolution empty, Plex fills it.

#include <cstdio>
#include <api/media/types.hpp>

using media::resolutionLabel;
using media::versionLabel;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static media::Media source(int width, int height, const char* codec, int64_t kbps) {
    media::Media m;
    m.width = width;
    m.height = height;
    m.videoCodec = codec;
    m.bitrate = kbps;
    return m;
}

int main() {
    CHECK(std::string(resolutionLabel(3840, 2160)) == "4K");
    CHECK(std::string(resolutionLabel(3840, 1600)) == "4K");  // scope
    CHECK(std::string(resolutionLabel(2560, 1440)) == "1440p");
    CHECK(std::string(resolutionLabel(1920, 1080)) == "1080p");
    CHECK(std::string(resolutionLabel(1920, 800)) == "1080p");
    CHECK(std::string(resolutionLabel(1440, 1080)) == "1080p");  // 4:3
    CHECK(std::string(resolutionLabel(1280, 536)) == "720p");
    CHECK(std::string(resolutionLabel(720, 480)) == "480p");
    CHECK(std::string(resolutionLabel(640, 360)) == "SD");
    CHECK(std::string(resolutionLabel(426, 240)) == "SD");
    CHECK(std::string(resolutionLabel(7680, 4320)) == "4K");
    CHECK(std::string(resolutionLabel(0, 0)).empty());

    CHECK(versionLabel(source(3840, 1600, "hevc", 23933)) == "4K · HEVC · 23.9 Mbps");
    CHECK(versionLabel(source(1920, 1080, "h264", 9769)) == "1080p · H264 · 9.8 Mbps");
    CHECK(versionLabel(source(720, 480, "mpeg2video", 3078)) == "480p · MPEG2VIDEO · 3.1 Mbps");
    CHECK(versionLabel(source(426, 240, "h264", 1087)) == "SD · H264 · 1.1 Mbps");
    CHECK(versionLabel(source(1920, 1080, "hevc", 0)) == "1080p · HEVC");

    // no size: Plex's own videoResolution
    media::Media plex = source(0, 0, "h264", 7607);
    plex.videoResolution = "1080";
    CHECK(versionLabel(plex) == "1080 · H264 · 7.6 Mbps");

    // nothing probed: the server's version name
    media::Media bare = source(0, 0, "", 0);
    CHECK(versionLabel(bare).empty());
    bare.label = "Patriot - Remux";
    CHECK(versionLabel(bare) == "Patriot - Remux");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_version_label: OK\n");
    return 0;
}
