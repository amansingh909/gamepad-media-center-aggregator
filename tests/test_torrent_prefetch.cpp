// Standalone logic test: when the source list prefetches a torrent (focus held a second,
// not twice in 90 s).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_prefetch.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include <torrent/prefetch_gate.hpp>

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
    {
        PrefetchGate g;
        g.focus("a", 0);
        CHECK(g.due(0.5) == "");   // not a second yet
        CHECK(g.due(0.96) == "a");  // a frame early is fine
        CHECK(g.due(1.5) == "");   // once per focus
        g.focus("a", 2);
        CHECK(g.due(3) == "");     // again within 90 s: no
        g.focus("a", 100);
        CHECK(g.due(101) == "a");  // after 90 s: again
    }
    {
        // scrolling past: only the row the focus stays on
        PrefetchGate g;
        g.focus("b", 0);
        g.focus("c", 0.6);
        CHECK(g.due(1.0) == "");   // c has had it 0.4 s
        CHECK(g.due(1.6) == "c");
        CHECK(g.due(5) == "");     // b never
    }
    {
        // focus moves away before the second is up
        PrefetchGate g;
        g.focus("d", 0);
        CHECK(g.blur("d"));
        CHECK(g.due(5) == "");
    }
    {
        // the next row gains focus before the last one hears it lost it
        PrefetchGate g;
        g.focus("e", 0);
        g.focus("f", 0.2);
        CHECK(!g.blur("e"));       // e wasn't waiting any more: f keeps its turn
        CHECK(g.due(1.2) == "f");
    }
    {
        // different rows one after the other
        PrefetchGate g;
        g.focus("g", 0);
        CHECK(g.due(1) == "g");
        g.focus("h", 1);
        CHECK(g.due(2) == "h");
    }
    {
        // a row that just played prefetches again at its next focus, even within 90 s
        PrefetchGate g;
        g.focus("i", 0);
        CHECK(g.due(1) == "i");
        g.forget("i");
        g.focus("i", 30);
        CHECK(g.due(31) == "i");
        g.forget("never seen");  // harmless
        g.focus("i", 40);
        CHECK(g.due(41) == "");  // and the 90 s rule holds again after that
    }
    if (failures == 0) printf("test_torrent_prefetch: all passed\n");
    return failures ? 1 : 0;
}
