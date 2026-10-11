// Standalone logic test: which pieces the torrent engine wants, and which get deadlines,
// for the readers it has open.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_plan.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <vector>
#include <torrent/piece_plan.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

using namespace torrent;

static const int P = 256 * 1024;
static const int64_t MiB = 1 << 20;

// every piece in [first, last] has priority `prio`
static bool all(const PiecePlan& plan, int first, int last, uint8_t prio) {
    if (last >= int(plan.priorities.size())) return false;
    for (int p = first; p <= last; ++p)
        if (plan.priorities[size_t(p)] != prio) return false;
    return true;
}

static int countTop(const PiecePlan& plan) {
    int n = 0;
    for (uint8_t p : plan.priorities) n += p == pieceTop;
    return n;
}

// the deadlines are exactly pieces [first, last], in order
static bool deadlinesCover(const std::vector<PieceDeadline>& d, size_t at, int first, int last) {
    if (d.size() < at + size_t(last - first + 1)) return false;
    for (int p = first; p <= last; ++p)
        if (d[at + size_t(p - first)].piece != p) return false;
    return true;
}

// the milliseconds of deadline `i`, or -1 if there is none (a failing plan must not crash the test)
static int msAt(const std::vector<PieceDeadline>& d, size_t i) { return i < d.size() ? d[i].ms : -1; }

static PlanReader reader(int64_t fileOffset, int64_t fileSize, int64_t windowStart, int64_t wantEnd,
                         int64_t position, int64_t deadlineEnd, double rate, bool waiting) {
    PlanReader r;
    r.fileOffset = fileOffset;
    r.fileSize = fileSize;
    r.windowStart = windowStart;
    r.wantEnd = wantEnd;
    r.position = position;
    r.deadlineEnd = deadlineEnd;
    r.rate = rate;
    r.waiting = waiting;
    return r;
}

int main() {
    // Two files in one 100 MiB torrent of 400 pieces:
    //   file 0: torrent bytes [0, 40 MiB), pieces 0-159, its last 2 MiB in pieces 152-159
    //   file 1: torrent bytes [40, 100 MiB), pieces 160-399, its last 2 MiB in pieces 392-399
    const int n = 400;
    const std::vector<bool> none(size_t(n), false);
    // A reads file 0 from its start; B reads file 1 from 10 MiB in (torrent piece 200)
    PlanReader a = reader(0, 40 * MiB, 0, 32 * MiB, 0, 4 * MiB, 0, true);
    PlanReader b = reader(40 * MiB, 60 * MiB, 10 * MiB, 42 * MiB, 10 * MiB, 14 * MiB, 2.0 * MiB, true);

    // two readers: both windows and both tails wanted
    PiecePlan plan = planPieces({a, b}, n, P, none);
    CHECK(plan.priorities.size() == size_t(n));
    CHECK(all(plan, 0, 127, pieceTop));     // A's window
    CHECK(all(plan, 128, 151, pieceSkip));
    CHECK(all(plan, 152, 159, pieceTop));   // A's tail
    CHECK(all(plan, 160, 199, pieceSkip));
    CHECK(all(plan, 200, 327, pieceTop));   // B's window
    CHECK(all(plan, 328, 391, pieceSkip));
    CHECK(all(plan, 392, 399, pieceTop));   // B's tail
    CHECK(countTop(plan) == 128 + 8 + 128 + 8);
    // both are waiting: 4 MiB of deadlines each, one per piece, in piece order
    CHECK(plan.deadlines.size() == 32);
    CHECK(deadlinesCover(plan.deadlines, 0, 0, 15));
    CHECK(deadlinesCover(plan.deadlines, 16, 200, 215));
    // milliseconds: bytes ahead of the reader over its rate, at least 1 MiB/s
    CHECK(msAt(plan.deadlines, 0) == 0);
    CHECK(msAt(plan.deadlines, 4) == 1000);  // A: 1 MiB ahead, no rate yet
    CHECK(msAt(plan.deadlines, 15) == 3750);
    CHECK(msAt(plan.deadlines, 16) == 0);    // B: at its position
    CHECK(msAt(plan.deadlines, 20) == 500);  // B: 1 MiB ahead at 2 MiB/s

    // release one of two: only the remaining reader's window and tail
    plan = planPieces({b}, n, P, none);
    CHECK(all(plan, 0, 199, pieceSkip));    // A's window and tail are gone
    CHECK(all(plan, 200, 327, pieceTop));
    CHECK(all(plan, 328, 391, pieceSkip));
    CHECK(all(plan, 392, 399, pieceTop));
    CHECK(countTop(plan) == 128 + 8);
    CHECK(plan.deadlines.size() == 16 && deadlinesCover(plan.deadlines, 0, 200, 215));

    // no readers left: nothing wanted
    plan = planPieces({}, n, P, none);
    CHECK(plan.priorities.size() == size_t(n) && countTop(plan) == 0 && plan.deadlines.empty());

    // an idle (not waiting) reader gets no deadlines but keeps its window
    PlanReader idle = a;
    idle.waiting = false;
    plan = planPieces({idle, b}, n, P, none);
    CHECK(all(plan, 0, 127, pieceTop) && all(plan, 152, 159, pieceTop));
    CHECK(all(plan, 200, 327, pieceTop) && all(plan, 392, 399, pieceTop));
    CHECK(countTop(plan) == 128 + 8 + 128 + 8);
    CHECK(plan.deadlines.size() == 16 && deadlinesCover(plan.deadlines, 0, 200, 215));

    // a reader that hasn't read anything yet wants only its file's tail
    PlanReader fresh = reader(40 * MiB, 60 * MiB, 0, 0, 0, 0, 0, false);
    plan = planPieces({fresh}, n, P, none);
    CHECK(countTop(plan) == 8 && all(plan, 392, 399, pieceTop) && plan.deadlines.empty());

    // deadlines skip pieces already had
    std::vector<bool> have(size_t(n), false);
    for (int p : {0, 1, 2, 3, 10}) have[size_t(p)] = true;
    plan = planPieces({a}, n, P, have);
    CHECK(plan.deadlines.size() == 11);
    CHECK(deadlinesCover(plan.deadlines, 0, 4, 9));
    CHECK(deadlinesCover(plan.deadlines, 6, 11, 15));
    CHECK(msAt(plan.deadlines, 0) == 1000);
    CHECK(all(plan, 0, 127, pieceTop));  // priorities don't depend on what is had

    // two waiting readers needing the same pieces: one deadline per piece, the earlier one
    PlanReader behind = reader(0, 40 * MiB, 0, 32 * MiB, 0, 4 * MiB, 0, true);
    PlanReader ahead = reader(0, 40 * MiB, 2 * MiB, 34 * MiB, 2 * MiB, 6 * MiB, 0, true);
    plan = planPieces({behind, ahead}, n, P, none);
    CHECK(plan.deadlines.size() == 24 && deadlinesCover(plan.deadlines, 0, 0, 23));
    CHECK(msAt(plan.deadlines, 8) == 0);      // piece 8 is where `ahead` reads (2000 ms for `behind`)
    CHECK(msAt(plan.deadlines, 15) == 1750);  // `ahead`: 1.75 MiB away (3750 ms for `behind`)

    // a window ending exactly at the end of a file whose size isn't a multiple of the
    // piece size: 21 pieces, the last one 17 bytes long
    {
        const int64_t size = 20 * int64_t(P) + 17;
        const int m = 21;
        PlanReader end = reader(0, size, 18 * int64_t(P), size, 18 * int64_t(P), size, 0, true);
        PiecePlan last = planPieces({end}, m, P, std::vector<bool>(size_t(m), false));
        CHECK(last.priorities.size() == size_t(m));
        CHECK(all(last, 0, 11, pieceSkip));
        CHECK(all(last, 12, 20, pieceTop));  // the tail, [size - 2 MiB, size), starts in piece 12
        CHECK(last.deadlines.size() == 3 && deadlinesCover(last.deadlines, 0, 18, 20));
        CHECK(msAt(last.deadlines, 0) == 0 && msAt(last.deadlines, 1) == 250 && msAt(last.deadlines, 2) == 500);
    }

    // a multi-file torrent whose file doesn't start on a piece boundary: a P/2 + 5 byte
    // file first, then a 10 MiB one (41 pieces in all)
    {
        const int64_t off = P / 2 + 5;
        const int m = 41;
        PlanReader r = reader(off, 10 * MiB, 0, 1 * MiB, 2 * int64_t(P), 3 * int64_t(P), 0, true);
        PiecePlan mf = planPieces({r}, m, P, std::vector<bool>(size_t(m), false));
        CHECK(all(mf, 0, 4, pieceTop));   // file bytes [0, 1 MiB) are torrent bytes [off, 1 MiB + off)
        CHECK(all(mf, 5, 31, pieceSkip));
        CHECK(all(mf, 32, 40, pieceTop));  // its last 2 MiB: torrent bytes [8 MiB + off, 10 MiB + off)
        // [2P, 3P) of the file is torrent [2P + off, 3P + off): pieces 2 and 3
        CHECK(mf.deadlines.size() == 2 && deadlinesCover(mf.deadlines, 0, 2, 3));
        CHECK(msAt(mf.deadlines, 0) == 0);
        CHECK(msAt(mf.deadlines, 1) == 124);  // piece 3 starts P/2 - 5 bytes ahead of the reader
    }

    if (failures == 0) printf("test_torrent_plan: all passed\n");
    return failures ? 1 : 0;
}
