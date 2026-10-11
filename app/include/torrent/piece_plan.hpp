// Which pieces of a torrent the engine downloads, and which of them get deadlines, for
// the readers it has open. The engine applies the whole plan when a reader's window
// refills or a reader leaves (so a seek, or a closed connection, takes the old position's
// pieces with it), and between refills only adds the deadlines it hasn't set yet.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <torrent/pieces.hpp>

namespace torrent {

/// One open reader, as the plan sees it.
struct PlanReader {
    int64_t fileOffset = 0, fileSize = 0;   // its file: where it starts in the torrent, its size
    int64_t windowStart = 0, wantEnd = 0;   // its window at its last refill: [windowStart, wantEnd)
    int64_t position = 0, deadlineEnd = 0;  // [position, deadlineEnd) is what it reads next
    double rate = 0;                        // bytes per second it has been reading
    bool waiting = false;                   // blocked in waitFor() right now
};

struct PieceDeadline {
    int piece = 0;
    int ms = 0;  // from now
};

/// Piece priorities, with libtorrent's values (the engine checks they match).
constexpr uint8_t pieceSkip = 0;  // dont_download
constexpr uint8_t pieceTop = 7;   // top_priority

/// The last bytes of every reader's file are always wanted: MP4 files often keep their
/// index at the end, and players read it first.
constexpr int64_t tailBytes = 2 << 20;

/// Deadlines assume the reader reads at least this fast, so one without a measured rate
/// yet still gets them a few seconds apart.
constexpr double minDeadlineRate = 1 << 20;

struct PiecePlan {
    std::vector<uint8_t> priorities;       // one per piece: pieceSkip or pieceTop
    std::vector<PieceDeadline> deadlines;  // at most one per piece, by piece index
};

/// Every reader's window and its file's tail at top priority, everything else skipped.
/// Deadlines only for readers that are waiting for data, on the pieces they read next
/// that aren't in `have` yet: an idle reader (its player paused, or a seek about to
/// close its connection) keeps its window downloading but doesn't hold the next pieces
/// of a waiting reader back. A piece two readers need gets the earlier deadline.
inline PiecePlan planPieces(const std::vector<PlanReader>& readers, int pieceCount, int pieceLength,
                            const std::vector<bool>& have) {
    PiecePlan plan;
    plan.priorities.assign(size_t(std::max(0, pieceCount)), pieceSkip);
    auto mark = [&](PieceSpan s) {
        for (int p = std::max(0, s.first); p <= s.last && p < pieceCount; ++p) plan.priorities[size_t(p)] = pieceTop;
    };
    for (const PlanReader& r : readers) {
        // [windowStart, wantEnd), not [position, wantEnd): a jump back inside the
        // reader's window doesn't refill it, so those pieces must stay wanted
        if (r.wantEnd > r.windowStart) mark(piecesFor(r.fileOffset, r.windowStart, r.wantEnd, pieceLength));
        mark(piecesFor(r.fileOffset, std::max<int64_t>(0, r.fileSize - tailBytes), r.fileSize, pieceLength));
    }
    for (const PlanReader& r : readers) {
        if (!r.waiting || r.deadlineEnd <= r.position) continue;
        PieceSpan next = piecesFor(r.fileOffset, r.position, r.deadlineEnd, pieceLength);
        double rate = std::max(r.rate, minDeadlineRate);
        for (int p = std::max(0, next.first); p <= next.last && p < pieceCount; ++p) {
            if (size_t(p) < have.size() && have[size_t(p)]) continue;
            int64_t ahead = std::max<int64_t>(0, int64_t(p) * pieceLength - r.fileOffset - r.position);
            plan.deadlines.push_back({p, int(double(ahead) / rate * 1000)});
        }
    }
    std::sort(plan.deadlines.begin(), plan.deadlines.end(), [](const PieceDeadline& a, const PieceDeadline& b) {
        return a.piece != b.piece ? a.piece < b.piece : a.ms < b.ms;
    });
    plan.deadlines.erase(std::unique(plan.deadlines.begin(), plan.deadlines.end(),
                                     [](const PieceDeadline& a, const PieceDeadline& b) { return a.piece == b.piece; }),
                         plan.deadlines.end());
    return plan;
}

}  // namespace torrent
