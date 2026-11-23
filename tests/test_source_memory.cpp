// Standalone logic test: the source a Stremio title last played from (utils/source_memory.hpp).
//
//   c++ -std=gnu++17 -Wall -Iapp/include -Ilibrary/borealis/library/include/borealis/extern tests/test_source_memory.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <string>

#include <utils/source_memory.hpp>

using namespace source_memory;

static int failures = 0;
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    const std::string local = "http://127.0.0.1:11470";
    // a torrent by its hash and file, whatever trackers came with it; a link as it is
    CHECK_EQ(sourceKey(local + "/abc/2?tr=udp%3A%2F%2Fa&f=x.mkv", local), local + "/abc/2");
    CHECK_EQ(sourceKey(local + "/abc/2", local), local + "/abc/2");
    CHECK_EQ(sourceKey("https://debrid.example/dl/x.mkv?token=1", local), "https://debrid.example/dl/x.mkv?token=1");
    CHECK_EQ(sourceKey(local + "/abc/2?tr=x", ""), local + "/abc/2?tr=x");  // no local server in this build

    // remember: most recent first, one entry per title, cut to the size
    nlohmann::json store = nlohmann::json::array();
    store = remember(store, "tt1", "a");
    store = remember(store, "tt2:1:1", "b");
    store = remember(store, "tt1", "c");  // tt1 again: moves first with its new source
    CHECK_EQ(store.dump(), R"([{"id":"tt1","source":"c"},{"id":"tt2:1:1","source":"b"}])");
    CHECK_EQ(recall(store, "tt1"), "c");
    CHECK_EQ(recall(store, "tt2:1:1"), "b");
    CHECK_EQ(recall(store, "tt3"), "");
    store = remember(store, "tt3", "d", 2);
    CHECK_EQ(store.dump(), R"([{"id":"tt3","source":"d"},{"id":"tt1","source":"c"}])");
    CHECK_EQ(remember(store, "", "x").dump(), store.dump());  // nothing to remember
    CHECK_EQ(remember(store, "tt9", "").dump(), store.dump());

    // a store that isn't one (a setting from elsewhere): read as empty
    CHECK_EQ(recall(nlohmann::json::object(), "tt1"), "");
    CHECK_EQ(remember(nlohmann::json("oops"), "tt1", "a").dump(), R"([{"id":"tt1","source":"a"}])");
    CHECK_EQ(recall(nlohmann::json::parse(R"([{"id":1,"source":"a"},{"id":"tt1"},{"id":"tt1","source":"b"}])"), "tt1"),
        "b");

    // finding it again among the sources of a new fetch
    CHECK_EQ(std::to_string(indexOf({"x", "", "c", "c"}, "c")), "2");
    CHECK_EQ(std::to_string(indexOf({"x", "y"}, "c")), "-1");
    CHECK_EQ(std::to_string(indexOf({"", "y"}, "")), "-1");  // no key: never the unplayable one

    if (failures == 0) printf("test_source_memory: all passed\n");
    return failures ? 1 : 0;
}
