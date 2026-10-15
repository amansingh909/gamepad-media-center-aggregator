// HTTP Range headers for the torrent server.
#pragma once

#include <cstdint>
#include <string>

namespace torrent {

struct ByteRange {
    int64_t first = 0;
    int64_t last = 0;  // inclusive
};

enum class RangeKind { Full, Partial, Unsatisfiable };

/// Parses "bytes=a-b", "bytes=a-" and "bytes=-n" against a file size. A missing,
/// malformed or multi-range header means the whole file (RFC 9110 lets a server
/// ignore those); `out` is then the whole file.
inline RangeKind parseRange(const std::string& header, int64_t size, ByteRange& out) {
    out.first = 0;
    out.last = size - 1;
    const std::string prefix = "bytes=";
    if (header.compare(0, prefix.size(), prefix) != 0) return RangeKind::Full;
    std::string spec = header.substr(prefix.size());
    if (spec.find(',') != std::string::npos) return RangeKind::Full;
    size_t dash = spec.find('-');
    if (dash == std::string::npos) return RangeKind::Full;
    std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
    auto digits = [](const std::string& s) {
        if (s.empty() || s.size() > 18) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    };

    if (a.empty()) {
        if (!digits(b)) return RangeKind::Full;
        int64_t n = std::stoll(b);
        if (n == 0 || size == 0) return RangeKind::Unsatisfiable;
        out.first = n >= size ? 0 : size - n;
        out.last = size - 1;
        return RangeKind::Partial;
    }
    if (!digits(a) || (!b.empty() && !digits(b))) return RangeKind::Full;
    int64_t first = std::stoll(a);
    if (first >= size) return RangeKind::Unsatisfiable;
    int64_t last = b.empty() ? size - 1 : std::stoll(b);
    if (last < first) return RangeKind::Full;
    if (last >= size) last = size - 1;
    out.first = first;
    out.last = last;
    return RangeKind::Partial;
}

}  // namespace torrent
