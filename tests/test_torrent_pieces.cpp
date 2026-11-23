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

    // a jump back to a spot the window has already moved past is a seek too
    ReadWindow back(1000 * MiB);
    back.update(0);
    p = back.update(20 * MiB);  // refill: the window now starts at 20 MiB
    CHECK(p.refill && p.wantEnd == 52 * MiB);
    p = back.update(5 * MiB);
    CHECK(p.refill && p.wantEnd == 37 * MiB);
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

    // big pieces: the window holds at least 8 of them, so there are always pieces past the
    // urgent ones to fill in the background, and deadlines cover at least 2, so the next piece
    // is urgent before the reader gets to it; small pieces keep the 32 MiB and 4 MiB floors
    {
        ReadWindow eight(1000 * MiB, 8 * MiB);
        p = eight.update(0);
        CHECK(p.refill && p.wantEnd == 64 * MiB && p.deadlineEnd == 16 * MiB);
        p = eight.update(30 * MiB);  // 34 MiB still ahead: more than half
        CHECK(!p.refill && p.wantEnd == 64 * MiB);
        p = eight.update(33 * MiB);
        CHECK(p.refill && p.wantEnd == 97 * MiB);
        ReadWindow sixteen(1000 * MiB, 16 * MiB);
        p = sixteen.update(0);
        CHECK(p.wantEnd == 128 * MiB && p.deadlineEnd == 32 * MiB);
        ReadWindow small(1000 * MiB, 256 << 10);
        p = small.update(0);
        CHECK(p.wantEnd == 32 * MiB && p.deadlineEnd == 4 * MiB);
        // 64 MiB pieces: 8 of them is more than the 256 MiB ceiling, and 2 more than the 32 MiB
        // deadline ceiling, which both give way
        ReadWindow sixtyFour(10000 * MiB, 64 * MiB);
        sixtyFour.noteRead(0, 0.0);
        sixtyFour.noteRead(2000 * MiB, 1.0);
        p = sixtyFour.update(0);
        CHECK(p.wantEnd == 512 * MiB && p.deadlineEnd == 128 * MiB);
        // a fast reader's window grows past 8 pieces as before, up to the ceiling
        ReadWindow fast16(10000 * MiB, 16 * MiB);
        fast16.noteRead(0, 0.0);
        fast16.noteRead(4 * MiB, 1.0);  // 4 MiB/s: 60 s is 240 MiB
        p = fast16.update(0);
        CHECK(p.refill && p.wantEnd == 240 * MiB && p.deadlineEnd == 32 * MiB);
    }

    // how much of a range is in: verified pieces whole, written blocks of the others
    {
        const int64_t B = blockBytes;  // 16 KiB, 16 blocks a piece
        const int64_t total = 10 * int64_t(P);
        std::vector<bool> have(10, false);
        CHECK(bytesIn(0, 0, 4 * int64_t(P), P, total, have, {}) == 0);
        have[0] = have[1] = true;
        CHECK(bytesIn(0, 0, 4 * int64_t(P), P, total, have, {}) == 2 * int64_t(P));
        // a file 1000 bytes into the torrent: its first P bytes are piece 0's tail and
        // piece 1's first 1000 bytes
        CHECK(bytesIn(1000, 0, P, P, total, have, {}) == P);
        CHECK(bytesIn(1000, 0, P, P, total, {true, false}, {}) == P - 1000);
        // piece 2 downloading: blocks 0, 1 and 5 written
        std::vector<bool> blocks(16, false);
        blocks[0] = blocks[1] = blocks[5] = true;
        const std::vector<PartialPiece> partial = {{2, blocks}};
        CHECK(bytesIn(0, 0, 4 * int64_t(P), P, total, have, partial) == 2 * int64_t(P) + 3 * B);
        // a range that starts and ends inside written blocks counts only its part
        CHECK(bytesIn(0, 2 * int64_t(P) + B / 2, 2 * int64_t(P) + B + B / 2, P, total, have, partial) == B);
        // a block or piece outside the range counts nothing
        CHECK(bytesIn(0, 2 * int64_t(P) + 2 * B, 2 * int64_t(P) + 5 * B, P, total, have, partial) == 0);
        CHECK(bytesIn(0, 3 * int64_t(P), 4 * int64_t(P), P, total, have, partial) == 0);
        // a piece both verified and in the download queue counts once
        have[2] = true;
        CHECK(bytesIn(0, 0, 4 * int64_t(P), P, total, have, partial) == 3 * int64_t(P));
        // never more than the range, never past the torrent, nothing for an empty range
        CHECK(bytesIn(0, -5, P, P, total, have, {}) == P);
        CHECK(bytesIn(0, 0, 20 * int64_t(P), P, total, std::vector<bool>(10, true), {}) == total);
        CHECK(bytesIn(0, 100, 100, P, total, have, {}) == 0);
        CHECK(bytesIn(0, 0, P, 0, total, have, {}) == 0);

        // a short last piece: 3 full pieces, then 40000 bytes (blocks of 16384, 16384, 7232)
        const int64_t shortTotal = 3 * int64_t(P) + 40000;
        std::vector<bool> none(4, false);
        CHECK(bytesIn(0, 0, shortTotal, P, shortTotal, {false, false, false, true}, {}) == 40000);
        CHECK(bytesIn(0, 0, shortTotal, P, shortTotal, none, {{3, {false, false, true}}}) == 7232);
        CHECK(bytesIn(0, 0, shortTotal, P, shortTotal, none, {{3, {true, true, true}}}) == 40000);
        // its file starts 2 bytes in: the file's last 4 bytes are the torrent's last 4
        CHECK(bytesIn(2, shortTotal - 6, shortTotal - 2, P, shortTotal, none, {{3, {false, false, true}}}) == 4);
    }

    // what a reader can read before it waits: verified pieces in a row from its position
    {
        const std::vector<bool> have = {true, true, false, true};
        CHECK(torrent::bytesAhead(0, 50, 400, 100, have) == 150);   // pieces 0 and 1
        CHECK(torrent::bytesAhead(0, 250, 400, 100, have) == 0);    // piece 2 is missing
        CHECK(torrent::bytesAhead(0, 300, 400, 100, have) == 100);  // the last piece, to the file's end
        CHECK(torrent::bytesAhead(30, 0, 300, 100, have) == 170);   // a file starting inside piece 0
        CHECK(torrent::bytesAhead(0, 400, 400, 100, have) == 0);    // at the end
        CHECK(torrent::bytesAhead(0, 50, 400, 100, {}) == 0);       // no metadata yet
    }
    if (failures == 0) printf("test_torrent_pieces: all passed\n");
    return failures ? 1 : 0;
}
