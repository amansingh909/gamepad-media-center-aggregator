// The Settings choices for the torrent upload limit, and the stored value's mapping to
// them. Header-only (tests/test_torrent_settings.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace torrent {

struct UploadLimitOption {
    const char* label;
    int bytesPerSec;
};

constexpr UploadLimitOption uploadLimitOptions[] = {
    {"256 KB/s", 256 << 10},
    {"512 KB/s", 512 << 10},
    {"1 MB/s", 1 << 20},
    {"2 MB/s", 2 << 20},
};
constexpr int uploadLimitCount = int(sizeof uploadLimitOptions / sizeof uploadLimitOptions[0]);
constexpr int defaultUploadLimitIndex = 2;
constexpr int defaultUploadLimit = uploadLimitOptions[defaultUploadLimitIndex].bytesPerSec;

inline std::vector<std::string> uploadLimitLabels() {
    std::vector<std::string> out;
    for (const auto& o : uploadLimitOptions) out.push_back(o.label);
    return out;
}

/// The choice a stored value is; the default (1 MB/s) for anything else, such as a
/// hand-edited or damaged config.
inline int uploadLimitIndex(int64_t bytesPerSec) {
    for (int i = 0; i < uploadLimitCount; ++i)
        if (uploadLimitOptions[i].bytesPerSec == bytesPerSec) return i;
    return defaultUploadLimitIndex;
}

/// The stored value as the engine takes it: one of the choices, else the default.
inline int uploadLimitValue(int64_t bytesPerSec) { return uploadLimitOptions[uploadLimitIndex(bytesPerSec)].bytesPerSec; }

}  // namespace torrent
