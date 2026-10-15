// Which part of a file to download ahead of its reader. From the PS4 spike: the
// window must be refilled in batches (continuous top-ups kept libtorrent in
// end-game mode, re-requesting blocks already in flight, and tripled the traffic),
// and only the stretch right ahead of the reader gets deadlines. The window is about
// 60 s of reading, between 32 MiB and 256 MiB, and at least 8 pieces: movie torrents use
// 8 to 16 MiB pieces, and 32 MiB of them leaves no pieces past the urgent ones to fill in
// the background (piece_plan.hpp). Deadlines cover about 10 s, between 4 MiB and 32 MiB, and
// at least 2 pieces: a piece is readable only once all of it is in, so the next one must be
// urgent before the reader gets to it.
#pragma once

#include <algorithm>
#include <cstdint>

namespace torrent {

/// The fewest pieces a window holds, and deadlines cover, however short the reading is.
constexpr int64_t minWindowPieces = 8;
constexpr int64_t minDeadlinePieces = 2;

class ReadWindow {
public:
    struct Plan {
        int64_t wantEnd;      // download [position, wantEnd)
        int64_t deadlineEnd;  // pieces in [position, deadlineEnd) get deadlines
        bool refill;          // wantEnd moved: re-prioritize
    };

    /// `pieceLength` (0: not known) raises the window's floor to minWindowPieces pieces and the
    /// deadlines' to minDeadlinePieces, and their ceilings with them when that is more.
    explicit ReadWindow(int64_t fileSize, int64_t pieceLength = 0, int64_t minWindow = 32ll << 20,
        double windowSecs = 60, double deadlineSecs = 10, int64_t minDeadline = 4ll << 20,
        int64_t maxWindow = 256ll << 20, int64_t maxDeadline = 32ll << 20)
        : fileSize(fileSize), minWindow(std::max(minWindow, minWindowPieces * pieceLength)), windowSecs(windowSecs),
          deadlineSecs(deadlineSecs), minDeadline(std::max(minDeadline, minDeadlinePieces * pieceLength)),
          maxWindow(std::max(maxWindow, minWindowPieces * pieceLength)),
          maxDeadline(std::max(maxDeadline, minDeadlinePieces * pieceLength)) {}

    /// The reader got `bytes` more at time `now` (seconds). The rate is sampled over
    /// at least a second, so bursts served from data already on disk don't read as
    /// gigabytes per second.
    void noteRead(int64_t bytes, double now) {
        if (sampleStart < 0) {
            sampleStart = now;
            return;
        }
        sampleBytes += bytes;
        double elapsed = now - sampleStart;
        if (elapsed < 1.0) return;
        double instant = double(sampleBytes) / elapsed;
        bytesPerSec = bytesPerSec <= 0 ? instant : bytesPerSec * 0.8 + instant * 0.2;
        sampleStart = now;
        sampleBytes = 0;
    }

    double rate() const { return bytesPerSec; }

    /// The reader is at `position`. A jump outside the window (a seek) restarts it.
    Plan update(int64_t position) {
        int64_t window = std::max(minWindow, int64_t(std::min(bytesPerSec * windowSecs, double(maxWindow))));
        int64_t deadline = std::max(minDeadline, int64_t(std::min(bytesPerSec * deadlineSecs, double(maxDeadline))));
        bool seek = position < windowStart || position > wantEnd;
        bool low = wantEnd - position < window / 2;
        int64_t newEnd = (seek || low) ? std::min(fileSize, position + window) : wantEnd;
        bool refill = seek || newEnd != wantEnd;
        if (refill) windowStart = position;  // [windowStart, wantEnd) is what the last refill prioritized
        wantEnd = newEnd;
        return {wantEnd, std::min(wantEnd, std::min(fileSize, position + deadline)), refill};
    }

private:
    int64_t fileSize, minWindow;
    double windowSecs, deadlineSecs;
    int64_t minDeadline, maxWindow, maxDeadline;
    double bytesPerSec = 0;
    double sampleStart = -1;
    int64_t sampleBytes = 0;
    int64_t windowStart = 0, wantEnd = 0;
};

}  // namespace torrent
