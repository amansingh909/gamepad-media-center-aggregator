// Mapping file bytes to torrent pieces, and choosing which file to play.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace torrent {

struct PieceSpan {
    int first = 0;
    int last = 0;  // inclusive
};

/// Pieces covering [begin, end) of a file that starts `fileOffset` bytes into the
/// torrent. An empty range names the piece holding `begin`.
inline PieceSpan piecesFor(int64_t fileOffset, int64_t begin, int64_t end, int pieceLength) {
    PieceSpan s;
    s.first = int((fileOffset + begin) / pieceLength);
    s.last = int((fileOffset + std::max(begin, end - 1)) / pieceLength);
    return s;
}

/// libtorrent's block: pieces download in blocks of this size, the torrent's very last
/// block can be shorter.
constexpr int64_t blockBytes = 16 << 10;

/// A piece still downloading: which of its blocks are written (or being written), in order.
struct PartialPiece {
    int piece = 0;
    std::vector<bool> written;
};

/// Bytes of [begin, end) of a file that starts `fileOffset` bytes into a torrent of
/// `totalSize` bytes that are in: the pieces in `have` whole, and the written blocks of
/// the pieces in `partial` that `have` doesn't already count. Clipped to the range and to
/// the torrent, so it never exceeds end - begin.
inline int64_t bytesIn(int64_t fileOffset, int64_t begin, int64_t end, int pieceLength, int64_t totalSize,
                       const std::vector<bool>& have, const std::vector<PartialPiece>& partial) {
    const int64_t lo = fileOffset + std::max<int64_t>(0, begin);
    const int64_t hi = std::min(fileOffset + end, totalSize);
    if (pieceLength <= 0 || hi <= lo) return 0;
    auto overlap = [lo, hi](int64_t a, int64_t b) { return std::max<int64_t>(0, std::min(b, hi) - std::max(a, lo)); };
    auto has = [&have](int p) { return p >= 0 && size_t(p) < have.size() && have[size_t(p)]; };
    const int first = int(lo / pieceLength), last = int((hi - 1) / pieceLength);
    int64_t in = 0;
    for (int p = first; p <= last; ++p)
        if (has(p)) in += overlap(int64_t(p) * pieceLength, std::min(int64_t(p + 1) * pieceLength, totalSize));
    for (const PartialPiece& pp : partial) {
        if (pp.piece < first || pp.piece > last || has(pp.piece)) continue;
        const int64_t start = int64_t(pp.piece) * pieceLength;
        const int64_t pieceEnd = std::min(start + pieceLength, totalSize);
        for (size_t b = 0; b < pp.written.size(); ++b) {
            if (!pp.written[b]) continue;
            const int64_t from = start + int64_t(b) * blockBytes;
            in += overlap(from, std::min(from + blockBytes, pieceEnd));
        }
    }
    return in;
}

/// Stremio's rule: an index picks that file, -1 picks the largest (the first one on
/// ties). Returns -1 when the request can't be served.
inline int pickFile(int requested, const std::vector<int64_t>& sizes) {
    if (requested >= 0) return requested < int(sizes.size()) ? requested : -1;
    if (requested != -1 || sizes.empty()) return -1;
    return int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
}

}  // namespace torrent
