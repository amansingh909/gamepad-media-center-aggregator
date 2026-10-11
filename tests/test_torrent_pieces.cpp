// Standalone logic test: piece math and the read-ahead window of the torrent engine.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_pieces.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <vector>
#include <torrent/pieces.hpp>
#include <torrent/read_window.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

int main() {
    using namespace torrent;
    const int P = 256 * 1024;

    // a file 1000 bytes into the torrent
    PieceSpan s = piecesFor(1000, 0, 100, P);
    CHECK(s.first == 0 && s.last == 0);
    s = piecesFor(1000, P - 1000, P - 999, P);  // first byte of piece 1
    CHECK(s.first == 1 && s.last == 1);
    s = piecesFor(0, 0, 3 * int64_t(P), P);  // exactly three pieces
    CHECK(s.first == 0 && s.last == 2);
    s = piecesFor(0, 10, 10, P);  // empty range still names its piece
    CHECK(s.first == 0 && s.last == 0);
    // a file whose size isn't a multiple of the piece size, read to its last byte
    const int64_t odd = 3 * int64_t(P) + 17;
    s = piecesFor(5, odd - 4096, odd, P);  // the last 4 KiB straddle pieces 2 and 3
    CHECK(s.first == 2 && s.last == 3);
    s = piecesFor(5, odd - 1, odd, P);  // the very last byte
    CHECK(s.first == 3 && s.last == 3);

    CHECK(pickFile(-1, {10, 500, 30}) == 1);
    CHECK(pickFile(-1, {7, 7}) == 0);
    CHECK(pickFile(2, {10, 500, 30}) == 2);
    CHECK(pickFile(3, {10, 500, 30}) == -1);
    CHECK(pickFile(-5, {10}) == -1);
    CHECK(pickFile(-1, {}) == -1);

    const int64_t MiB = 1 << 20;
    ReadWindow w(1000 * MiB);
    ReadWindow::Plan p = w.update(0);
    CHECK(p.refill && p.wantEnd == 32 * MiB && p.deadlineEnd == 4 * MiB);
    p = w.update(10 * MiB);  // more than half the window still ahead
    CHECK(!p.refill && p.wantEnd == 32 * MiB);
    p = w.update(20 * MiB);  // under half: refill in one batch
    CHECK(p.refill && p.wantEnd == 52 * MiB);
    p = w.update(500 * MiB);  // seek forward past the window
    CHECK(p.refill && p.wantEnd == 532 * MiB);
    p = w.update(100 * MiB);  // seek backward
    CHECK(p.refill && p.wantEnd == 132 * MiB);
    p = w.update(990 * MiB);  // near the end: clamped to the file
    CHECK(p.refill && p.wantEnd == 1000 * MiB && p.deadlineEnd == 994 * MiB);
    p = w.update(995 * MiB);  // nothing left to add
    CHECK(!p.refill && p.wantEnd == 1000 * MiB && p.deadlineEnd == 999 * MiB);

    ReadWindow fast(10000 * MiB);
    fast.noteRead(MiB, 0.0);
    fast.noteRead(MiB, 1.0);  // 1 MiB/s
    CHECK(fast.rate() > 0.99 * MiB && fast.rate() < 1.01 * MiB);
    fast.noteRead(4 * MiB, 2.0);  // smoothed toward 4 MiB/s
    CHECK(fast.rate() > 1.59 * MiB && fast.rate() < 1.61 * MiB);
    p = fast.update(0);  // 60 s at 1.6 MiB/s beats the 32 MiB floor
    CHECK(p.refill && p.wantEnd > 95 * MiB && p.wantEnd < 97 * MiB);
    CHECK(p.deadlineEnd > 15 * MiB && p.deadlineEnd < 17 * MiB);

    // reads served from disk arrive microseconds apart: no rate until a full second
    ReadWindow burst(10000 * MiB);
    burst.noteRead(256 << 10, 0.0);
    burst.noteRead(256 << 10, 0.00005);
    burst.noteRead(256 << 10, 0.0001);
    CHECK(burst.rate() == 0);
    burst.noteRead(MiB, 1.0001);  // 1.5 MiB over 1.0001 s
    CHECK(burst.rate() > 1.49 * MiB && burst.rate() < 1.51 * MiB);

    // a reader far faster than any swarm: window and deadlines stay bounded
    ReadWindow huge(10000 * MiB);
    huge.noteRead(0, 0.0);
    huge.noteRead(2000 * MiB, 1.0);
    p = huge.update(0);
    CHECK(p.refill && p.wantEnd == 256 * MiB && p.deadlineEnd == 32 * MiB);

    if (failures == 0) printf("test_torrent_pieces: all passed\n");
    return failures ? 1 : 0;
}
