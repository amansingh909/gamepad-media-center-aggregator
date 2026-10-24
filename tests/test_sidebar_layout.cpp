// Standalone logic test — per-server sidebar layout.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_sidebar_layout.cpp -o /tmp/t && /tmp/t
//
// Exercises the PURE layout logic (sidebar_layout.hpp): resolving the saved
// layout against the tabs present, and saving an edit without losing the ids
// that are absent right now (offline, libraries not downloaded).

#include <cstdio>
#include <utils/sidebar_layout.hpp>

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

using Ids = std::vector<std::string>;
using Set = std::set<std::string>;

int main() {
    {
        // saved ids first, new tabs (search, files) appended in natural order
        Ids order;
        Set hidden;
        sidebar::resolve({"lib/3", "lib/5", "tab/playlists", "tab/search", "tab/remote"},
            {"lib/5", "lib/3", "lib/9", "tab/playlists"}, {"lib/3", "lib/9"}, order, hidden);
        CHECK((order == Ids {"lib/5", "lib/3", "tab/playlists", "tab/search", "tab/remote"}));
        CHECK((hidden == Set {"lib/3"}));  // lib/9 is not present
    }

    {
        // offline: only lib/5 downloaded; moving search first keeps the order
        // and the hidden state of the libraries that are not there
        Ids out;
        Set outHidden;
        sidebar::merge({"lib/5", "lib/741", "lib/3", "lib/729", "tab/playlists"}, {"lib/3"},
            {"tab/search", "lib/5", "tab/playlists"}, {}, out, outHidden);
        CHECK((out == Ids {"tab/search", "lib/5", "lib/741", "lib/3", "lib/729", "tab/playlists"}));
        CHECK((outHidden == Set {"lib/3"}));
    }

    {
        // an absent id follows the id saved right before it when that one moves
        Ids out;
        Set outHidden;
        sidebar::merge({"lib/5", "lib/741", "lib/3", "tab/playlists"}, {}, {"lib/3", "tab/playlists", "lib/5"}, {},
            out, outHidden);
        CHECK((out == Ids {"lib/3", "tab/playlists", "lib/5", "lib/741"}));
    }

    {
        // absent ids leading the saved order stay in front, in their order
        Ids out;
        Set outHidden;
        sidebar::merge({"lib/1", "lib/2", "lib/5"}, {}, {"lib/5"}, {"lib/5"}, out, outHidden);
        CHECK((out == Ids {"lib/1", "lib/2", "lib/5"}));
        CHECK((outHidden == Set {"lib/5"}));
    }

    {
        // a present tab shown again leaves the hidden set; nothing saved yet
        Ids out;
        Set outHidden;
        sidebar::merge({}, {}, {"lib/1", "tab/search"}, {"tab/search"}, out, outHidden);
        CHECK((out == Ids {"lib/1", "tab/search"}));
        CHECK((outHidden == Set {"tab/search"}));
        sidebar::merge({"lib/1", "tab/search"}, {"tab/search"}, {"lib/1", "tab/search"}, {}, out, outHidden);
        CHECK(outHidden.empty());
    }

    {
        // duplicated ids in a hand-edited config do not duplicate tabs
        Ids out;
        Set outHidden;
        sidebar::merge({"lib/1", "lib/2", "lib/1"}, {}, {"lib/2"}, {}, out, outHidden);
        CHECK((out == Ids {"lib/1", "lib/2"}));
    }

    if (failures == 0) {
        printf("test_sidebar_layout: OK\n");
        return 0;
    }
    printf("test_sidebar_layout: %d FAILURE(S)\n", failures);
    return 1;
}
