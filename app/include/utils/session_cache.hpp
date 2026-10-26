// Values asked once per key and kept for the app session: everyone asking while a key's request
// is in flight is answered by that one request. And a walk over keys that applies the kept ones at
// once and asks for the others one at a time, so that on a single FIFO worker (brls::async) a
// request the user makes meanwhile waits for one of the walk's at most. Used for a show's
// episodes (StremioBackend::getAllEpisodes, the Home row's Continue Watching cards).
// Single-threaded: every call on one thread (the UI thread). STL only (tests/test_session_cache.cpp).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cache {

template <class T>
class SessionCache {
public:
    /// An answer: the value, or nullptr and the request's error.
    using Answer = std::function<void(const T* value, const std::string& error)>;

    /// `fetch(key)` starts the one request for `key`; it answers later through done() or failed().
    explicit SessionCache(std::function<void(const std::string&)> fetch) : fetch(std::move(fetch)) {}

    /// The value kept for `key`, or nullptr.
    const T* find(const std::string& key) const {
        auto it = this->values.find(key);
        return it == this->values.end() ? nullptr : &it->second;
    }

    /// True while the request for `key` is in flight.
    bool asking(const std::string& key) const { return this->waiting.count(key) > 0; }

    /// Answers with `key`'s value: at once when it is kept, else once the request for it answers
    /// (started here when none is in flight).
    void get(const std::string& key, Answer answer) {
        if (const T* kept = this->find(key)) {
            answer(kept, "");
            return;
        }
        auto it = this->waiting.find(key);
        if (it != this->waiting.end()) {
            it->second.push_back(std::move(answer));
            return;
        }
        this->waiting[key].push_back(std::move(answer));
        this->fetch(key);
    }

    /// The request for `key` answered: the value is kept, and everyone waiting gets it.
    void done(const std::string& key, T value) {
        auto& kept = this->values[key];
        kept = std::move(value);
        for (auto& answer : this->take(key)) answer(&kept, "");
    }

    /// The request for `key` failed: nothing is kept (the next get() asks again), and everyone
    /// waiting gets the error.
    void failed(const std::string& key, const std::string& error) {
        for (auto& answer : this->take(key)) answer(nullptr, error);
    }

private:
    // the answers waiting on `key`, out of the map first: an answer may ask for another key
    std::vector<Answer> take(const std::string& key) {
        std::vector<Answer> out;
        auto it = this->waiting.find(key);
        if (it == this->waiting.end()) return out;
        out = std::move(it->second);
        this->waiting.erase(it);
        return out;
    }

    std::function<void(const std::string&)> fetch;
    std::map<std::string, T> values;  // never erased: pointers to them stay valid
    std::map<std::string, std::vector<Answer>> waiting;
};

namespace detail {

template <class T>
struct Walk {
    std::vector<std::string> keys;
    size_t next = 0;
    std::function<const T*(const std::string&)> kept;
    std::function<void(const std::string&, typename SessionCache<T>::Answer)> ask;
    std::function<bool()> alive;
    std::function<void(const std::string&, const T*, const std::string&)> each;
};

template <class T>
void walkFrom(const std::shared_ptr<Walk<T>>& w) {
    while (w->next < w->keys.size()) {
        if (!w->alive()) return;
        const std::string key = w->keys[w->next++];
        if (const T* value = w->kept(key)) {
            w->each(key, value, "");
            continue;
        }
        w->ask(key, [w, key](const T* value, const std::string& error) {
            if (!w->alive()) return;
            w->each(key, value, error);
            walkFrom(w);
        });
        return;
    }
}

}  // namespace detail

/// Goes through `keys` in order while `alive()` holds: a key whose value is `kept` goes to `each`
/// at once; the first one that isn't is asked (`ask`, which answers later), and the walk goes on
/// from there once it has answered, so it never has more than one request in flight. A failed
/// key goes to `each` with nullptr and its error, and the walk goes on. Once `alive()` is false
/// nothing more is asked or applied.
template <class T>
void walkOneAtATime(std::vector<std::string> keys, std::function<const T*(const std::string&)> kept,
    std::function<void(const std::string&, typename SessionCache<T>::Answer)> ask, std::function<bool()> alive,
    std::function<void(const std::string&, const T*, const std::string&)> each) {
    auto w = std::make_shared<detail::Walk<T>>();
    w->keys = std::move(keys);
    w->kept = std::move(kept);
    w->ask = std::move(ask);
    w->alive = std::move(alive);
    w->each = std::move(each);
    detail::walkFrom(w);
}

}  // namespace cache
