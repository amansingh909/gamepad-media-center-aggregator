#pragma once

/*
    How curl's data reaches the stream a request writes into (a download's file, a body in
    memory). curl takes any count other than the one it passed as a write error: it stops
    the transfer with CURLE_WRITE_ERROR, and the request throws. So once the stream has
    failed (a full disk), the transfer fails instead of finishing a truncated file that
    looks complete. STL only, so tests/test_http_write.cpp can exercise it.
*/

#include <cstddef>
#include <ostream>

/// Writes `count` bytes to `out`: `count` if the stream is still good afterwards, else 0.
inline size_t writeChunk(std::ostream& out, const char* data, size_t count) {
    out.write(data, std::streamsize(count));
    return out.good() ? count : 0;
}
