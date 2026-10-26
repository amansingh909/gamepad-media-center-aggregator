// When a torrent row says "May buffer": the file's bitrate (its size over the title's length)
// clearly above the download speed the engine has seen recently. Header-only
// (tests/test_torrent_buffer.cpp).
#pragma once

#include <cstdint>

namespace torrent {

/// Samples below this many bytes per second (64 KiB/s) say nothing about the link: a full
/// window or a throttled grace torrent downloads little because it needs little.
constexpr int64_t minRateSample = 64 * 1024;

/// The recent download speed after one more sample, in bytes per second: a slow average (a
/// tenth per sample) of the samples that moved data (at least minRateSample). 0 until one did.
inline int64_t nextRecentRate(int64_t recent, int64_t sample) {
    if (sample < minRateSample) return recent;
    if (recent <= 0) return sample;
    return (recent * 9 + sample) / 10;
}

/// True when a file of `sizeBytes` played over `durationMs` needs more than a quarter above
/// `rateBytesPerSec`: playback would stop to buffer. False when any of them isn't known.
inline bool mayBuffer(int64_t sizeBytes, int64_t durationMs, int64_t rateBytesPerSec) {
    if (sizeBytes <= 0 || durationMs <= 0 || rateBytesPerSec <= 0) return false;
    const int64_t bitrate = sizeBytes * 1000 / durationMs;  // bytes per second
    return bitrate * 4 > rateBytesPerSec * 5;
}

}  // namespace torrent
