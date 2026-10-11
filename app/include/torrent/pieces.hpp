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

/// Stremio's rule: an index picks that file, -1 picks the largest (the first one on
/// ties). Returns -1 when the request can't be served.
inline int pickFile(int requested, const std::vector<int64_t>& sizes) {
    if (requested >= 0) return requested < int(sizes.size()) ? requested : -1;
    if (requested != -1 || sizes.empty()) return -1;
    return int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
}

}  // namespace torrent
