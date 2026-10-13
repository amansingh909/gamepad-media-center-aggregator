// Which pieces of a torrent the engine downloads, at which priority, and which of them get
// deadlines, for the readers it has open. A piece is readable only once all of it is in and
// verified, and movie torrents use 8 to 16 MiB pieces: with a whole window at one priority
// the swarm spread over ~9 of them and the one a reader waited for came in late (GMCA diag H:
// readers waiting with 0.1 MB ahead while 4.5 MB/s came in). So what a waiting reader reads
// next (its next ~10 s and at least the next piece, read_window.hpp; at most 3 big pieces) is
// urgent: top priority and deadlines, the first due now. The rest of its window fills in the
// background, at low priority, in file order (the engine downloads in libtorrent's sequential
// mode).
// The engine applies the whole plan when a reader's window refills or a reader leaves (so a
// seek, or a closed connection, takes the old position's pieces with it), and between refills
// only adds the deadlines it hasn't set yet (libtorrent raises a piece with a deadline to top
// priority).
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
constexpr uint8_t pieceSkip = 0;        // dont_download
constexpr uint8_t pieceBackground = 1;  // low_priority
constexpr uint8_t pieceTop = 7;         // top_priority

/// Pieces this big hold the shortest deadline span (4 MiB, read_window.hpp) on their own: at
/// most maxUrgentPieces of them are urgent for a reader, so the swarm finishes the first.
constexpr int64_t bigPieceBytes = 4 << 20;
constexpr int maxUrgentPieces = 3;

/// The last bytes of every reader's file are always wanted: MP4 files often keep their
/// index at the end, and players read it first.
constexpr int64_t tailBytes = 2 << 20;

/// Deadlines assume the reader reads at least this fast, so one without a measured rate
/// yet still gets them a few seconds apart.
constexpr double minDeadlineRate = 1 << 20;

struct PiecePlan {
    std::vector<uint8_t> priorities;       // one per piece: pieceSkip, pieceBackground or pieceTop
    std::vector<PieceDeadline> deadlines;  // at most one per piece, by piece index
};

/// The pieces a waiting reader needs next: those of [position, deadlineEnd), at most
/// maxUrgentPieces of them when pieces are big.
inline PieceSpan urgentPieces(const PlanReader& r, int pieceLength) {
    PieceSpan s = piecesFor(r.fileOffset, r.position, r.deadlineEnd, pieceLength);
    if (pieceLength >= bigPieceBytes) s.last = std::min(s.last, s.first + maxUrgentPieces - 1);
    return s;
}

/// Every reader's window in the background and its file's tail at top priority, everything
/// else skipped. What waiting readers need next (urgentPieces()) at top priority, with
/// deadlines on those that aren't in `have` yet: an idle reader (its player paused, or a seek
/// about to close its connection) keeps its window downloading but doesn't hold the next
/// pieces of a waiting reader back. A piece two readers want gets the higher priority and the
/// earlier deadline.
inline PiecePlan planPieces(const std::vector<PlanReader>& readers, int pieceCount, int pieceLength,
                            const std::vector<bool>& have) {
    PiecePlan plan;
    plan.priorities.assign(size_t(std::max(0, pieceCount)), pieceSkip);
    auto mark = [&](PieceSpan s, uint8_t prio) {
        for (int p = std::max(0, s.first); p <= s.last && p < pieceCount; ++p)
            plan.priorities[size_t(p)] = std::max(plan.priorities[size_t(p)], prio);
    };
    for (const PlanReader& r : readers) {
        // [windowStart, wantEnd), not [position, wantEnd): a jump back inside the
        // reader's window doesn't refill it, so those pieces must stay wanted
        if (r.wantEnd > r.windowStart)
            mark(piecesFor(r.fileOffset, r.windowStart, r.wantEnd, pieceLength), pieceBackground);
        mark(piecesFor(r.fileOffset, std::max<int64_t>(0, r.fileSize - tailBytes), r.fileSize, pieceLength), pieceTop);
    }
    for (const PlanReader& r : readers) {
        if (!r.waiting || r.deadlineEnd <= r.position) continue;
        const PieceSpan next = urgentPieces(r, pieceLength);
        mark(next, pieceTop);
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

/// Whether the deadlines set so far (`deadlined`, one flag per piece) must go before the plan
/// whose deadlines are `next` is applied: one is on a piece that isn't in and that no waiting
/// reader needs next any more (a seek, a reader gone or idle). libtorrent clears deadlines all
/// together, and when the first one is set again it cancels every request in flight that isn't
/// for a piece with a deadline: a refill that only moves forward keeps them.
inline bool staleDeadlines(const std::vector<bool>& deadlined, const std::vector<bool>& have,
                           const std::vector<PieceDeadline>& next) {
    std::vector<bool> urgent(deadlined.size(), false);
    for (const PieceDeadline& d : next)
        if (d.piece >= 0 && size_t(d.piece) < urgent.size()) urgent[size_t(d.piece)] = true;
    for (size_t i = 0; i < deadlined.size(); ++i)
        if (deadlined[i] && !(i < have.size() && have[i]) && !urgent[i]) return true;
    return false;
}

}  // namespace torrent
