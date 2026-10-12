#include <torrent/torrent_service.hpp>

#include <torrent/torrent_server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace torrent {

namespace {

// a warm-up waits in the server's chunk size, so its progress moves in these steps
const int64_t warmupChunk = 256 << 10;

std::atomic<TorrentService*> currentService{nullptr};

EngineSettings engineSettings(const ServiceSettings& s) {
    EngineSettings e;
    e.tempDir = s.tempDir;
    e.uploadLimit = s.uploadLimit;
    e.stateFile = s.stateFile;
    e.log = s.log;
    return e;
}

}  // namespace

struct Warmup::State {
    std::atomic<int64_t> buffered{0};
    std::atomic<int64_t> target{0};
    // the worker's: progress() without the buffered floor, from what the engine says of
    // `reader` while the warm-up waits for its bytes (`reading`)
    std::atomic<int64_t> progress{0};
    std::atomic<bool> reading{false};
    std::atomic<ReaderId> reader{0};
    std::atomic<bool> abandoned{false};  // the engine reads it with its lock held: no mutex
    std::atomic<bool> exited{false};     // its thread is done and can be joined
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;  // finish() or cancel(): the reader can go

    void end(bool cancel) {
        if (cancel) abandoned = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

Warmup::Warmup(std::shared_ptr<State> s) : state(std::move(s)) {}
int64_t Warmup::buffered() const { return state->buffered; }
int64_t Warmup::target() const { return state->target; }
int64_t Warmup::progress() const { return std::max<int64_t>(state->progress, state->buffered); }
void Warmup::finish() { state->end(false); }
void Warmup::cancel() { state->end(true); }
// Dropping the handle ends the warm-up: end(true) both gives up a warm-up that hasn't reported
// and, once `done` has run (abandoned is no longer read), lets the thread release its reader.
Warmup::~Warmup() { state->end(true); }

struct TorrentService::Impl {
    ServiceSettings settings;
    TorrentEngine engine;  // declared before the server, which must go first
    TorrentServer server;
    std::atomic<bool> enabled;
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::string startError;

    // the worker: prefetches, upload limit changes, and the watched torrent's stats
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<StreamRequest> prefetches;
    int uploadLimit = -1;  // a limit to apply, -1 for none
    int keepOpen = -1;     // keep the engine's session open (1) or let it idle (0), -1 for no change
    std::string watched;
    Stats latest;
    bool quit = false;
    std::thread worker;

    // warm-ups, a thread each, joined once they're done or by stop()
    struct Running {
        std::thread thread;
        std::shared_ptr<Warmup::State> state;
        StreamRequest req;
    };
    std::mutex warmMutex;
    std::list<Running> warmups;

    explicit Impl(ServiceSettings s)
        : settings(std::move(s)), engine(engineSettings(settings)), server(engine, settings.log),
          enabled(settings.enabled) {
        server.setEnabled(settings.enabled);
        worker = std::thread([this] { run(); });
    }

    void log(const std::string& m) {
        if (settings.log) settings.log(m);
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!quit) {
            wake.wait_for(lock, std::chrono::milliseconds(500),
                          [this] { return quit || !prefetches.empty() || uploadLimit >= 0 || keepOpen >= 0; });
            if (quit) break;
            std::deque<StreamRequest> todo;
            todo.swap(prefetches);
            const int limit = uploadLimit;
            uploadLimit = -1;
            const int keep = keepOpen;
            keepOpen = -1;
            const std::string hash = watched;
            lock.unlock();
            // these log their own failures and never throw; a session starting takes a moment,
            // which is why it starts here and not on the caller's thread
            if (keep >= 0) engine.keepSessionOpen(keep == 1);
            if (limit >= 0) {
                engine.setUploadLimit(limit);
                log("upload limit " + std::to_string(limit / 1024) + " KB/s");
            }
            for (const auto& r : todo) {
                log("prefetch " + r.infoHash);
                engine.prefetch(r);
            }
            Stats s = hash.empty() ? Stats{} : engine.stats(hash);
            publishProgress();
            lock.lock();
            if (hash == watched) latest = keepLastError(s, latest);
        }
    }

    // Warmup::progress(): what's in so far of each buffering warm-up's [0, target),
    // written blocks included. The engine call waits on libtorrent's thread, so it runs
    // with no lock of ours held (warmUp() takes warmMutex on the UI thread).
    void publishProgress() {
        std::vector<std::pair<StreamRequest, std::shared_ptr<Warmup::State>>> reading;
        {
            std::lock_guard<std::mutex> lock(warmMutex);
            for (const Running& r : warmups)
                if (r.state->reading) reading.emplace_back(r.req, r.state);
        }
        for (const auto& w : reading) {
            Warmup::State& st = *w.second;
            const int64_t target = st.target;
            const int64_t ready = target > 0 ? engine.bytesReady(w.first, st.reader, 0, target) : -1;
            // this thread is the only writer: the value only goes up
            if (ready > st.progress) st.progress = ready;
        }
    }

    void runWarmup(const StreamRequest& req, int64_t bytes, const std::shared_ptr<Warmup::State>& st,
                   const std::function<void(Status)>& done) {
        const auto began = std::chrono::steady_clock::now();
        bool told = false;
        auto tell = [&](Status s) {
            if (told) return;
            told = true;
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began);
            log("warm-up " + req.infoHash + ": " + statusName(s) + " after " + std::to_string(secs.count()) + " s");
            try {
                done(s);
            } catch (const std::exception& e) {
                log(std::string("warm-up callback: ") + e.what());
            } catch (...) {
                log("warm-up callback failed");
            }
        };
        try {
            const std::function<bool()> abandoned = [&st] { return st->abandoned.load(); };
            FileInfo file;
            ReaderId reader = 0;
            Status result = engine.acquire(req, file, reader, abandoned);
            if (result == Status::Ok) {
                struct Release {
                    TorrentEngine& engine;
                    const StreamRequest& req;
                    ReaderId reader;
                    ~Release() { engine.release(req, reader); }
                } release{engine, req, reader};
                const int64_t target = std::min(bytes, file.size);
                st->target = target;
                st->reader = reader;
                st->reading = true;
                for (int64_t pos = 0; pos < target && result == Status::Ok; pos += warmupChunk) {
                    const int64_t end = std::min(target, pos + warmupChunk);
                    result = engine.waitFor(req, reader, pos, end, abandoned);
                    if (result == Status::Ok) st->buffered = end;
                }
                st->reading = false;
                tell(result);
                if (result == Status::Ok) {
                    // the player opens the file now: keep the reader (its window and the
                    // file's tail stay wanted) until mpv has data or the player goes
                    std::unique_lock<std::mutex> lock(st->mutex);
                    st->changed.wait(lock, [&st] { return st->released; });
                }
            }
            tell(result);
        } catch (const std::exception& e) {
            log(std::string("warm-up: ") + e.what());
            tell(Status::Failed);
        } catch (...) {
            log("warm-up failed");
            tell(Status::Failed);
        }
        st->exited = true;
    }

    // joins the warm-ups that are done; warmMutex held
    void reap() {
        for (auto it = warmups.begin(); it != warmups.end();) {
            if (it->state->exited) {
                if (it->thread.joinable()) it->thread.join();
                it = warmups.erase(it);
            } else {
                ++it;
            }
        }
    }
};

TorrentService::TorrentService(ServiceSettings settings) : impl(std::make_unique<Impl>(std::move(settings))) {}

TorrentService::~TorrentService() { stop(); }

bool TorrentService::start() {
    if (impl->stopped) return false;
    if (impl->started) return true;
    std::string error;
    if (!impl->server.start(impl->settings.port, impl->settings.lan, error)) {
        impl->startError = error;
        impl->log("not serving: " + error);
        return false;
    }
    impl->started = true;
    // the engine's session starts now, on the worker, and stays open while streaming is on:
    // the DHT is warm when the first torrent comes
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->keepOpen = impl->enabled ? 1 : 0;
    }
    impl->wake.notify_one();
    return true;
}

std::string TorrentService::startError() const { return impl->startError; }

void TorrentService::stop() {
    if (impl->stopped.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(impl->warmMutex);
        for (auto& w : impl->warmups) w.state->end(true);
    }
    impl->server.stop();      // ends the open streams
    impl->engine.shutdown();  // every wait ends with Stopped, server or not
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->quit = true;
    }
    impl->wake.notify_all();
    if (impl->worker.joinable()) impl->worker.join();
    std::list<Impl::Running> all;
    {
        std::lock_guard<std::mutex> lock(impl->warmMutex);
        all.swap(impl->warmups);
    }
    for (auto& w : all)
        if (w.thread.joinable()) w.thread.join();
}

Availability TorrentService::availability() const {
    if (impl->stopped) return Availability::Stopped;
    if (!impl->started) return Availability::NotStarted;
    return impl->enabled ? Availability::Ready : Availability::Off;
}

void TorrentService::setEnabled(bool on) {
    impl->enabled = on;
    impl->server.setEnabled(on);
    impl->log(on ? "streaming on" : "streaming off");
    if (!impl->started) return;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->keepOpen = on ? 1 : 0;
    }
    impl->wake.notify_one();
}

void TorrentService::setUploadLimit(int bytesPerSec) {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->uploadLimit = bytesPerSec;
    }
    impl->wake.notify_one();
}

void TorrentService::prefetch(const StreamRequest& req) {
    if (availability() != Availability::Ready) return;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (const auto& r : impl->prefetches)
            if (r.infoHash == req.infoHash) return;
        impl->prefetches.push_back(req);
    }
    impl->wake.notify_one();
}

void TorrentService::watch(const std::string& infoHash) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->watched == infoHash) return;
    impl->watched = infoHash;
    impl->latest = Stats{};
}

Stats TorrentService::snapshot() const {
    std::lock_guard<std::mutex> lock(impl->mutex);
    return impl->latest;
}

std::shared_ptr<Warmup> TorrentService::warmUp(const StreamRequest& req, int64_t bytes,
                                               std::function<void(Status)> done) {
    auto state = std::make_shared<Warmup::State>();
    auto handle = std::make_shared<Warmup>(state);
    if (availability() != Availability::Ready) {
        done(Status::Stopped);
        return handle;
    }
    Status refused = Status::Ok;  // set when no thread was made; `done` runs after the lock is released
    {
        std::lock_guard<std::mutex> lock(impl->warmMutex);
        // stop() sets `stopped` before it takes this mutex, so a thread added here is
        // either seen by stop() or never made: none is left unjoined
        if (impl->stopped) {
            refused = Status::Stopped;
        } else {
            impl->reap();
            try {
                impl->warmups.emplace_back();
                Impl::Running& running = impl->warmups.back();
                running.state = state;
                running.req = req;
                running.thread =
                    std::thread([this, req, bytes, state, done] { impl->runWarmup(req, bytes, state, done); });
            } catch (const std::exception& e) {  // system_error, bad_alloc
                // a Running without a thread must not stay in the list
                if (!impl->warmups.empty() && !impl->warmups.back().thread.joinable()) impl->warmups.pop_back();
                impl->log(std::string("no thread for a warm-up: ") + e.what());
                refused = Status::Failed;
            }
        }
    }
    if (refused != Status::Ok) done(refused);
    return handle;
}

TorrentService* TorrentService::current() { return currentService.load(); }

void TorrentService::setCurrent(TorrentService* service) { currentService.store(service); }

}  // namespace torrent
