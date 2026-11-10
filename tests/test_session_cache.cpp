// Standalone logic test: values asked once per key and kept for the app session, with everyone
// waiting on a request in flight answered by it, and a walk over keys that keeps at most one of
// its requests in flight (session_cache.hpp: a show's episodes for the Home row's cards).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_session_cache.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <utils/session_cache.hpp>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

using Cache = cache::SessionCache<std::string>;

static std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : ",") + s;
    return out;
}

// A walk over a cache, with its requests in a list the test answers when it wants
struct Walker {
    std::vector<std::string> fetched;  // every request the cache started, in order
    Cache c{[this](const std::string& key) { this->fetched.push_back(key); }};
    std::vector<std::string> seen;  // what the walk applied: "key=value" or "key!error"
    bool alive = true;

    void walk(const std::vector<std::string>& keys, std::vector<std::string>* out = nullptr) {
        std::vector<std::string>* to = out ? out : &this->seen;
        const bool* live = &this->alive;
        cache::walkOneAtATime<std::string>(
            keys, [this](const std::string& k) { return this->c.find(k); },
            [this](const std::string& k, Cache::Answer a) { this->c.get(k, std::move(a)); },
            [live]() { return *live; },
            [to](const std::string& k, const std::string* v, const std::string& err) {
                to->push_back(v ? k + "=" + *v : k + "!" + err);
            });
    }
};

int main() {
    // one request per key, everyone waiting on it answered by it, then kept
    {
        std::vector<std::string> fetched;
        Cache c([&fetched](const std::string& key) { fetched.push_back(key); });
        CHECK(c.find("a") == nullptr);
        std::vector<std::string> got;
        auto answer = [&got](const std::string* v, const std::string& err) { got.push_back(v ? *v : "!" + err); };
        c.get("a", answer);
        c.get("a", answer);  // in flight: waits on the same request
        CHECK_EQ(join(fetched), "a");
        CHECK(c.asking("a"));
        CHECK(got.empty());
        c.done("a", "eps-a");
        CHECK_EQ(join(got), "eps-a,eps-a");
        CHECK(!c.asking("a"));
        CHECK(c.find("a") != nullptr && *c.find("a") == "eps-a");
        c.get("a", answer);  // kept: answered at once, no request
        CHECK_EQ(join(got), "eps-a,eps-a,eps-a");
        CHECK_EQ(join(fetched), "a");
    }

    // a failed request keeps nothing, answers everyone waiting with its error, and is asked again
    {
        std::vector<std::string> fetched;
        Cache c([&fetched](const std::string& key) { fetched.push_back(key); });
        std::vector<std::string> got;
        auto answer = [&got](const std::string* v, const std::string& err) { got.push_back(v ? *v : "!" + err); };
        c.get("b", answer);
        c.get("b", answer);
        c.failed("b", "http status 500");
        CHECK_EQ(join(got), "!http status 500,!http status 500");
        CHECK(c.find("b") == nullptr);
        CHECK(!c.asking("b"));
        c.get("b", answer);
        CHECK_EQ(join(fetched), "b,b");
        c.done("b", "eps-b");
        CHECK_EQ(join(got), "!http status 500,!http status 500,eps-b");
    }

    // an answer that asks for another key (a walk going on) starts that request at once
    {
        std::vector<std::string> fetched;
        Cache c([&fetched](const std::string& key) { fetched.push_back(key); });
        std::vector<std::string> got;
        c.get("a", [&](const std::string* v, const std::string&) {
            got.push_back(*v);
            c.get("b", [&](const std::string* w, const std::string&) { got.push_back(*w); });
        });
        c.done("a", "1");
        CHECK_EQ(join(fetched), "a,b");
        c.done("b", "2");
        CHECK_EQ(join(got), "1,2");
    }

    // a walk with nothing kept: one request at a time, in the keys' order
    {
        Walker w;
        w.walk({"a", "b", "c"});
        CHECK_EQ(join(w.fetched), "a");
        CHECK(w.seen.empty());
        w.c.done("a", "1");
        CHECK_EQ(join(w.seen), "a=1");
        CHECK_EQ(join(w.fetched), "a,b");  // the next one only once the first answered
        w.c.done("b", "2");
        CHECK_EQ(join(w.fetched), "a,b,c");
        w.c.done("c", "3");
        CHECK_EQ(join(w.seen), "a=1,b=2,c=3");
        CHECK_EQ(join(w.fetched), "a,b,c");
    }

    // everything kept (a row rebuilt after the player closed): all applied at once, no request
    {
        Walker w;
        w.c.done("a", "1");
        w.c.done("b", "2");
        w.walk({"a", "b"});
        CHECK_EQ(join(w.seen), "a=1,b=2");
        CHECK(w.fetched.empty());
    }

    // kept ones go at once up to the first that isn't, the rest after its answer
    {
        Walker w;
        w.c.done("a", "1");
        w.c.done("c", "3");
        w.walk({"a", "b", "c", "d"});
        CHECK_EQ(join(w.seen), "a=1");
        CHECK_EQ(join(w.fetched), "b");
        w.c.done("b", "2");
        CHECK_EQ(join(w.seen), "a=1,b=2,c=3");
        CHECK_EQ(join(w.fetched), "b,d");
        w.c.done("d", "4");
        CHECK_EQ(join(w.seen), "a=1,b=2,c=3,d=4");
    }

    // a failed key is reported and the walk goes on
    {
        Walker w;
        w.walk({"a", "b"});
        w.c.failed("a", "timeout");
        CHECK_EQ(join(w.seen), "a!timeout");
        CHECK_EQ(join(w.fetched), "a,b");
        w.c.done("b", "2");
        CHECK_EQ(join(w.seen), "a!timeout,b=2");
    }

    // the row goes (rebuilt): the walk stops, but what was asked is still kept
    {
        Walker w;
        w.walk({"a", "b"});
        w.alive = false;
        w.c.done("a", "1");
        CHECK(w.seen.empty());
        CHECK_EQ(join(w.fetched), "a");  // nothing more asked
        CHECK(w.c.find("a") != nullptr);
    }

    // the rebuilt row's walk waits on the old walk's request instead of asking again
    {
        Walker w;
        w.walk({"a", "b"});
        w.alive = false;  // the old row goes...
        std::vector<std::string> second;
        bool alive2 = true;
        cache::walkOneAtATime<std::string>(
            {"a", "b"}, [&w](const std::string& k) { return w.c.find(k); },
            [&w](const std::string& k, Cache::Answer a) { w.c.get(k, std::move(a)); }, [&alive2]() { return alive2; },
            [&second](const std::string& k, const std::string* v, const std::string&) {
                second.push_back(k + "=" + *v);
            });
        CHECK_EQ(join(w.fetched), "a");  // ...and the new one waits on its "a"
        w.c.done("a", "1");
        CHECK(w.seen.empty());
        CHECK_EQ(join(second), "a=1");
        CHECK_EQ(join(w.fetched), "a,b");
        w.c.done("b", "2");
        CHECK_EQ(join(second), "a=1,b=2");
    }

    // a user's own ask for a key the walk hasn't reached: one request, and the walk then has it kept
    {
        Walker w;
        w.walk({"a", "b", "c"});
        std::vector<std::string> user;
        w.c.get("c", [&user](const std::string* v, const std::string&) { user.push_back(*v); });
        CHECK_EQ(join(w.fetched), "a,c");
        w.c.done("c", "3");
        CHECK_EQ(join(user), "3");
        w.c.done("a", "1");
        w.c.done("b", "2");
        CHECK_EQ(join(w.seen), "a=1,b=2,c=3");
        CHECK_EQ(join(w.fetched), "a,c,b");  // "c" was never asked twice
    }

    // an empty walk does nothing
    {
        Walker w;
        w.walk({});
        CHECK(w.seen.empty() && w.fetched.empty());
    }

    if (failures == 0) printf("test_session_cache: all passed\n");
    return failures ? 1 : 0;
}
