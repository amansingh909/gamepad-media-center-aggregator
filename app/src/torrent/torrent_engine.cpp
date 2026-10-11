#include <torrent/torrent_engine.hpp>

#include <torrent/piece_plan.hpp>
#include <torrent/pieces.hpp>
#include <torrent/read_window.hpp>
#include <torrent/temp_folder.hpp>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#ifdef __PS4__
#include <libtorrent/enum_net.hpp>
#include <orbis/NetCtl.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace torrent {

namespace lt = libtorrent;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static_assert(static_cast<std::uint8_t>(lt::dont_download) == pieceSkip &&
                  static_cast<std::uint8_t>(lt::top_priority) == pieceTop,
              "piece_plan.hpp's priorities are libtorrent's");

namespace {

double secondsNow() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

#ifdef __PS4__
// libtorrent can't list interfaces or routes on the PS4 (the TORRENT_PS4 patch), so
// hand it what NetCtl reports. borealis has already called sceNetCtlInit.
void setPs4Network(const std::function<void(const std::string&)>& log) {
    OrbisNetCtlInfo ip{}, mask{}, gw{};
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &ip) < 0 ||
        sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_NETMASK, &mask) < 0 ||
        sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_DEFAULT_ROUTE, &gw) < 0) {
        if (log) log("no network info from NetCtl");
        return;
    }
    lt::error_code e1, e2, e3;
    auto a = lt::make_address(ip.ip_address, e1);
    auto m = lt::make_address(mask.netmask, e2);
    auto g = lt::make_address(gw.default_route, e3);
    if (e1 || e2 || e3) {
        if (log) log("unparsable NetCtl addresses");
        return;
    }
    lt::ps4_set_network(a, m, g);
}
#endif

}  // namespace

// Per-torrent state. Not in the anonymous namespace: TorrentEngine::Impl has external
// linkage and holds these (GCC's -Wsubobject-linkage).
namespace detail {

// One reader of one file, with its own download window: two readers of one torrent
// (a player reading the start while it probes the end) must not undo each other.
struct Reader {
    int64_t fileOffset = 0, fileSize = 0;
    ReadWindow window;
    int64_t position = 0;               // where the last waitFor() began
    int64_t windowStart = 0;            // position at this reader's last refill
    ReadWindow::Plan plan{0, 0, false};  // what the window wanted then
    bool waiting = false;               // inside waitFor(): only then does it get deadlines

    Reader(int64_t offset, int64_t size) : fileOffset(offset), fileSize(size), window(size) {}
};

struct Torrent {
    lt::torrent_handle handle;
    std::string savePath;
    bool metadata = false;
    std::shared_ptr<const lt::torrent_info> info;  // set with `metadata`
    std::string error;  // set by a file or torrent error alert
    int readers = 0;    // readers in `active`, plus callers still waiting in acquire()
    Clock::time_point removeAt = Clock::time_point::max();
    Clock::time_point lastProgress = Clock::now();
    std::vector<bool> have;
    std::vector<bool> deadlined;  // got a deadline since the last reprioritize()
    int pieceLength = 1;
    std::map<ReaderId, Reader> active;
};

}  // namespace detail

using detail::Reader;
using detail::Torrent;

struct TorrentEngine::Impl {
    EngineSettings settings;
    const bool tempOk;  // an absolute tempDir; without one the engine creates and deletes nothing
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<lt::session> session;
    std::map<std::string, Torrent> torrents;
    std::vector<std::pair<std::string, Clock::time_point>> pendingDirs;
    Clock::time_point idleSince = Clock::now();
    bool quit = false;
    bool closing = false;  // expire() is destroying an idle session with the lock dropped
    int inside = 0;        // acquire() and waitFor() calls in progress
    ReaderId nextReader = 1;
    uint64_t addCount = 0;  // every add gets its own folder
    std::thread worker;

    // Counts a call that can wait on `changed`, so ~Impl doesn't free the torrents
    // under a waiter that stop() woke but that hasn't re-taken the lock yet. Declare
    // it right after the lock: it then ends while the lock is still held.
    struct Inside {
        Impl& impl;
        explicit Inside(Impl& i) : impl(i) { ++impl.inside; }
        ~Inside() {
            --impl.inside;
            impl.changed.notify_all();
        }
        Inside(const Inside&) = delete;
        Inside& operator=(const Inside&) = delete;
    };

    // Marks a reader as waiting for data for as long as a waitFor() call lasts. Declare
    // it after Inside, so it ends while the lock is still held. It looks the reader up
    // again at the end: the reader, or its torrent, may be gone by then.
    struct Waiting {
        Impl& impl;
        const std::string& hash;
        ReaderId id;
        Waiting(Impl& i, const std::string& h, ReaderId r) : impl(i), hash(h), id(r) { set(true); }
        ~Waiting() { set(false); }
        void set(bool on) {
            if (Torrent* t = impl.find(hash))
                if (Reader* r = impl.findReader(*t, id)) r->waiting = on;
        }
        Waiting(const Waiting&) = delete;
        Waiting& operator=(const Waiting&) = delete;
    };

    // One reader reference on a torrent, taken by acquire(): it keeps the torrent in
    // the map (and its address valid) while acquire() waits, and goes back on every
    // way out of acquire() but success, exceptions included. On success the new reader
    // owns it until release().
    struct Reference {
        Impl& impl;
        Torrent& t;
        bool kept = false;
        Reference(Impl& i, Torrent& tor) : impl(i), t(tor) { ++t.readers; }
        ~Reference() {
            if (!kept) impl.dropReader(t);
        }
        Reference(const Reference&) = delete;
        Reference& operator=(const Reference&) = delete;
    };

    explicit Impl(EngineSettings s) : settings(std::move(s)), tempOk(isUsableTempDir(settings.tempDir)) {
        if (!tempOk) log("temp folder \"" + settings.tempDir + "\" is not an absolute path: torrents are off");
        sweep();
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        stop();
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this] { return inside == 0; });
        }
        worker.join();
        std::unique_ptr<lt::session> s;
        {
            std::lock_guard<std::mutex> lock(mutex);
            s = std::move(session);
            torrents.clear();
        }
        s.reset();  // closes the session, so its files are no longer in use
        sweep();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        changed.notify_all();
    }

    void log(const std::string& m) {
        if (settings.log) settings.log(m);
    }

    void logError(const char* what, const char* detail) noexcept {
        try {
            log(std::string(what) + ": " + detail);
        } catch (...) {
        }
    }

    // Runs `f` and logs what it throws instead of letting it out: an exception that
    // escapes the worker thread, a UI-thread call or a destructor ends the app (a
    // libtorrent system_error, a bad_alloc). Returns false if `f` threw.
    template <class F>
    bool guarded(const char* what, F&& f) noexcept {
        try {
            f();
            return true;
        } catch (const std::exception& e) {
            logError(what, e.what());
        } catch (...) {
            logError(what, "unknown error");
        }
        return false;
    }

    // Deletes one of the engine's own save folders (<info hash>-<n>), never anything else.
    bool removeFolder(const fs::path& p) {
        if (!isEngineFolderName(p.filename().string())) {
            log("not deleting " + p.string() + ": not an engine folder");
            return false;
        }
        std::error_code ec;
        fs::remove_all(p, ec);
        if (ec) log("deleting " + p.string() + ": " + ec.message());
        return !ec;
    }

    // Deletes what a crash or an earlier run left in tempDir. Only the engine's own
    // folders: anything else in there is left alone.
    void sweep() {
        if (!tempOk) return;
        std::error_code ec;
        fs::create_directories(settings.tempDir, ec);
        if (ec) log("creating " + settings.tempDir + ": " + ec.message());
        std::vector<fs::path> leftovers;
        for (auto it = fs::directory_iterator(settings.tempDir, ec); !ec && it != fs::directory_iterator();
             it.increment(ec))
            if (isEngineFolderName(it->path().filename().string())) leftovers.push_back(it->path());
        if (ec) log("listing " + settings.tempDir + ": " + ec.message());
        int removed = 0;
        for (const auto& p : leftovers) removed += removeFolder(p) ? 1 : 0;
        if (removed > 0) log("swept " + std::to_string(removed) + " leftovers");
    }

    void startSession() {
#ifdef __PS4__
        setPs4Network(settings.log);
#endif
        lt::settings_pack sp;
        sp.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881");
        // block_progress: a torrent with big pieces that downloads slowly can go a minute
        // without finishing a piece while still moving; a written block is progress too
        sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::status | lt::alert_category::error |
                                                     lt::alert_category::storage |
                                                     lt::alert_category::piece_progress |
                                                     lt::alert_category::block_progress);
        // room for one alert per 16 KiB block between two pops of the queue
        sp.set_int(lt::settings_pack::alert_queue_size, 10000);
        sp.set_bool(lt::settings_pack::enable_dht, true);
        sp.set_bool(lt::settings_pack::enable_lsd, false);
        sp.set_bool(lt::settings_pack::enable_upnp, false);
        sp.set_bool(lt::settings_pack::enable_natpmp, false);
        sp.set_int(lt::settings_pack::connections_limit, 100);
        // a streaming torrent looks finished whenever its window is full; keep the seeds
        sp.set_bool(lt::settings_pack::close_redundant_connections, false);
        sp.set_int(lt::settings_pack::upload_rate_limit, settings.uploadLimit);
        // closing the session waits this long for trackers to acknowledge "stopped"
        sp.set_int(lt::settings_pack::stop_tracker_timeout, 1);
        lt::session_params params(std::move(sp));
        // The server relies on this: once waitFor() returns Ok, pread() of the file sees
        // the verified bytes. The POSIX backend writes each block to the file before
        // completing it and hashes a piece by reading it back from the file, so a piece
        // that passed is on disk. The default mmap backend can hash queued blocks from
        // memory before they reach the file. It is also the PS4 build's backend.
        params.disk_io_constructor = lt::posix_disk_io_constructor;
        session = std::make_unique<lt::session>(std::move(params));
        idleSince = Clock::now();
        log("session started");
    }

    Torrent* find(const std::string& hash) {
        auto it = torrents.find(hash);
        return it == torrents.end() ? nullptr : &it->second;
    }

    Torrent* byHandle(const lt::torrent_handle& h) {
        for (auto& kv : torrents)
            if (kv.second.handle == h) return &kv.second;
        return nullptr;
    }

    Reader* findReader(Torrent& t, ReaderId id) {
        auto it = t.active.find(id);
        return it == t.active.end() ? nullptr : &it->second;
    }

    Torrent* ensure(const StreamRequest& req, std::unique_lock<std::mutex>& lock, std::string& error) {
        // a session closing for idleness still holds the port: let it go first
        changed.wait(lock, [this] { return !closing || quit; });
        if (quit) {
            error = "engine stopped";
            return nullptr;
        }
        if (!tempOk) {
            error = "no usable temp folder";
            return nullptr;
        }
        if (Torrent* t = find(req.infoHash)) return t;
        if (!session) startSession();
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(magnetUri(req), ec);
        if (ec) {
            error = ec.message();
            return nullptr;
        }
        Torrent t;
        // its own folder: the previous copy of this torrent may still be being deleted
        t.savePath = settings.tempDir + "/" + req.infoHash + "-" + std::to_string(++addCount);
        atp.save_path = t.savePath;
        // no per-torrent upload limit here: a new or prefetched torrent has every piece
        // at dont_download, so it has nothing to upload, and libtorrent's per-torrent
        // limit also throttles handshakes and metadata requests (prefetch would never
        // get the metadata)
        // not auto-managed: libtorrent's queue would pause torrents past active_downloads
        // (3) or active_seeds (5), so a few dead or prefetched sources could hold the one
        // being watched paused until acquire() gives up
        atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused);
        t.handle = session->add_torrent(std::move(atp), ec);
        if (ec) {
            error = ec.message();
            return nullptr;
        }
        log("added " + req.infoHash);
        return &torrents.emplace(req.infoHash, std::move(t)).first->second;
    }

    // One place to let go of a reader reference: the last one starts the grace period
    // and stops uploading. The torrent now holds pieces it could share; 1 B/s also
    // holds back our own requests (the limit covers every byte sent to its peers),
    // which is fine with nobody reading. acquire() lifts it.
    void dropReader(Torrent& t) {
        if (t.readers <= 0) return;
        if (--t.readers > 0) return;
        t.removeAt = Clock::now() + std::chrono::seconds(settings.graceSecs);
        // runs in Reference's destructor: log a failure, don't throw it
        guarded("engine", [&] { t.handle.set_upload_limit(1); });
    }

    void onMetadata(Torrent& t) {
        if (t.metadata) return;
        auto ti = t.handle.torrent_file();
        if (!ti) return;
        t.info = ti;
        t.have.assign(size_t(ti->num_pieces()), false);
        t.deadlined.assign(size_t(ti->num_pieces()), false);
        t.pieceLength = ti->piece_length();
        // nothing downloads until a reader asks for a range
        t.handle.prioritize_pieces(std::vector<lt::download_priority_t>(size_t(ti->num_pieces()), lt::dont_download));
        t.metadata = true;
        t.lastProgress = Clock::now();
        log("metadata " + ti->name());
    }

    void handle(lt::alert* a) {
        if (auto* b = lt::alert_cast<lt::block_finished_alert>(a)) {
            // progress only: a piece is readable once piece_finished_alert says so
            if (Torrent* t = byHandle(b->handle)) t->lastProgress = Clock::now();
        } else if (auto* m = lt::alert_cast<lt::metadata_received_alert>(a)) {
            if (Torrent* t = byHandle(m->handle)) onMetadata(*t);
        } else if (auto* p = lt::alert_cast<lt::piece_finished_alert>(a)) {
            if (Torrent* t = byHandle(p->handle)) {
                size_t i = size_t(static_cast<int>(p->piece_index));
                if (i < t->have.size()) t->have[i] = true;
                t->lastProgress = Clock::now();
            }
        } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(a)) {
            if (Torrent* t = byHandle(fe->handle)) {
                t->error = fe->message();
                log("file error: " + t->error);
            }
        } else if (auto* te = lt::alert_cast<lt::torrent_error_alert>(a)) {
            if (Torrent* t = byHandle(te->handle)) {
                t->error = te->message();
                log("torrent error: " + t->error);
            }
        } else if (lt::alert_cast<lt::alerts_dropped_alert>(a)) {
            // a lost metadata or piece alert would leave readers waiting for data that
            // is already there: ask libtorrent instead
            log("alerts dropped, reloading piece state");
            for (auto& kv : torrents) {
                Torrent& t = kv.second;
                lt::torrent_status st = t.handle.status(lt::torrent_handle::query_pieces);
                if (st.has_metadata) onMetadata(t);
                for (int i = 0; i < int(t.have.size()) && i < st.pieces.size(); ++i)
                    if (st.pieces[lt::piece_index_t(i)]) t.have[size_t(i)] = true;
            }
        }
    }

    PiecePlan plan(const Torrent& t) {
        std::vector<PlanReader> readers;
        for (const auto& kv : t.active) {
            const Reader& r = kv.second;
            PlanReader p;
            p.fileOffset = r.fileOffset;
            p.fileSize = r.fileSize;
            p.windowStart = r.windowStart;
            p.wantEnd = r.plan.wantEnd;
            p.position = r.position;
            p.deadlineEnd = r.plan.deadlineEnd;
            p.rate = r.window.rate();
            p.waiting = r.waiting;
            readers.push_back(p);
        }
        return planPieces(readers, int(t.have.size()), t.pieceLength, t.have);
    }

    void addDeadlines(Torrent& t, const std::vector<PieceDeadline>& deadlines) {
        for (const PieceDeadline& d : deadlines) {
            if (t.deadlined[size_t(d.piece)]) continue;
            t.handle.set_piece_deadline(lt::piece_index_t(d.piece), d.ms);
            t.deadlined[size_t(d.piece)] = true;
        }
    }

    // Priorities and deadlines from scratch, for the readers open now: when a reader's
    // window refills and when a reader leaves, so a seek or a closed connection takes
    // the old position's pieces and deadlines with it.
    void reprioritize(Torrent& t) {
        PiecePlan p = plan(t);
        std::vector<lt::download_priority_t> prio;
        prio.reserve(p.priorities.size());
        for (uint8_t v : p.priorities) prio.push_back(lt::download_priority_t(v));
        // clear first: libtorrent sets every piece whose deadline it clears to low
        // priority, which would override the priorities below
        t.handle.clear_piece_deadlines();
        t.handle.prioritize_pieces(prio);
        std::fill(t.deadlined.begin(), t.deadlined.end(), false);
        addDeadlines(t, p.deadlines);
    }

    // Moves reader `r` to `position`. Priorities change only when some reader's window
    // refills (in one batch, as the spike learned) or a reader leaves; in between, the
    // waiting readers' deadlines are topped up as they move.
    void applyWindow(Torrent& t, Reader& r, int64_t position) {
        ReadWindow::Plan next = r.window.update(position);
        r.position = position;
        r.plan = next;
        if (next.refill) {
            r.windowStart = position;
            reprioritize(t);
        } else {
            addDeadlines(t, plan(t).deadlines);
        }
    }

    void expire(std::unique_lock<std::mutex>& lock) {
        auto now = Clock::now();
        for (auto it = torrents.begin(); it != torrents.end();) {
            Torrent& t = it->second;
            if (t.readers == 0 && now >= t.removeAt) {
                log("removing " + it->first);
                if (session) session->remove_torrent(t.handle, lt::session::delete_files);
                pendingDirs.emplace_back(t.savePath, now + std::chrono::seconds(5));
                it = torrents.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pendingDirs.begin(); it != pendingDirs.end();) {
            if (now >= it->second) {
                removeFolder(it->first);
                it = pendingDirs.erase(it);
            } else {
                ++it;
            }
        }
        if (!torrents.empty() || !session) {
            idleSince = now;
            return;
        }
        if (now - idleSince > std::chrono::seconds(settings.idleSecs)) {
            std::unique_ptr<lt::session> s = std::move(session);
            log("idle, closing the session");
            closing = true;
            lock.unlock();
            s.reset();
            lock.lock();
            closing = false;
            changed.notify_all();
        }
    }

    // One round of the worker: alerts, then expiry.
    void step(std::unique_lock<std::mutex>& lock) {
        lt::session* s = session.get();
        if (!s) {
            changed.wait_for(lock, std::chrono::milliseconds(250));
            expire(lock);
            return;
        }
        // only this thread destroys the session, so `s` stays valid unlocked
        lock.unlock();
        s->wait_for_alert(lt::milliseconds(250));
        std::vector<lt::alert*> alerts;
        s->pop_alerts(&alerts);
        lock.lock();
        // one at a time: an alert that fails mustn't lose the ones after it (a lost
        // piece alert leaves its reader waiting for data that is on disk)
        for (lt::alert* a : alerts) guarded("engine", [&] { handle(a); });
        if (!alerts.empty()) changed.notify_all();
        expire(lock);
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        int failures = 0;  // in a row
        while (!quit) {
            if (guarded("engine", [&] { step(lock); })) {
                failures = 0;
                continue;
            }
            // it may have thrown with the lock dropped
            if (!lock.owns_lock()) lock.lock();
            // a fault that keeps coming back must not spin: wait a little longer each
            // time, up to 5 s, and stop waiting at once on shutdown
            failures = std::min(failures + 1, 20);
            changed.wait_for(lock, std::chrono::milliseconds(250 * failures), [this] { return quit; });
        }
    }
};

TorrentEngine::TorrentEngine(EngineSettings settings) : impl(std::make_unique<Impl>(std::move(settings))) {}

TorrentEngine::~TorrentEngine() = default;

void TorrentEngine::shutdown() { impl->stop(); }

Status TorrentEngine::acquire(const StreamRequest& req, FileInfo& file, ReaderId& reader) {
    std::unique_lock<std::mutex> lock(impl->mutex);
    Impl::Inside inside(*impl);
    if (impl->quit) return Status::Stopped;
    std::string error;
    Torrent* t = impl->ensure(req, lock, error);
    if (!t) {
        if (impl->quit) return Status::Stopped;
        impl->log("add failed: " + error);
        return Status::BadRequest;
    }
    // declared after Inside: it gives the reference back while the lock is still held
    Impl::Reference ref(*impl, *t);
    // lift dropReader()'s 1 B/s throttle: the session-wide cap governs while someone reads
    if (t->readers == 1) t->handle.set_upload_limit(-1);
    t->removeAt = Clock::time_point::max();

    auto deadline = Clock::now() + std::chrono::seconds(impl->settings.stallSecs);
    while (!t->metadata && t->error.empty() && !impl->quit) {
        if (impl->changed.wait_until(lock, deadline) == std::cv_status::timeout && !t->metadata)
            return Status::NoPeers;
    }
    if (impl->quit) return Status::Stopped;
    if (!t->error.empty()) return Status::DiskError;

    const lt::file_storage& files = t->info->files();
    std::vector<int64_t> sizes;
    for (int i = 0; i < files.num_files(); ++i) sizes.push_back(files.file_size(lt::file_index_t(i)));
    int index = pickFile(req.fileIndex, sizes);
    if (index < 0) return Status::NotFound;

    // everything that can throw before the reader exists, so a throw leaves nothing behind
    lt::file_index_t fi(index);
    FileInfo found;
    found.name = files.file_path(fi);
    found.diskPath = files.file_path(fi, t->savePath);
    found.size = files.file_size(fi);
    ReaderId id = impl->nextReader++;
    t->active.emplace(std::piecewise_construct, std::forward_as_tuple(id),
                      std::forward_as_tuple(files.file_offset(fi), files.file_size(fi)));
    ref.kept = true;
    reader = id;
    file = std::move(found);
    return Status::Ok;
}

void TorrentEngine::release(const StreamRequest& req, ReaderId reader) {
    // the server calls this from a destructor, where a throw would end the app
    impl->guarded("release", [&] {
        std::lock_guard<std::mutex> lock(impl->mutex);
        Torrent* t = impl->find(req.infoHash);
        if (!t || !impl->findReader(*t, reader)) return;
        t->active.erase(reader);
        impl->dropReader(*t);
        // the readers left get the torrent to themselves: the released window and its
        // deadlines would otherwise stay until one of them refills
        if (!t->active.empty()) impl->reprioritize(*t);
    });
}

Status TorrentEngine::waitFor(const StreamRequest& req, ReaderId reader, int64_t begin, int64_t end) {
    std::unique_lock<std::mutex> lock(impl->mutex);
    Impl::Inside inside(*impl);
    if (impl->quit) return Status::Stopped;
    Torrent* t = impl->find(req.infoHash);
    Reader* r = t ? impl->findReader(*t, reader) : nullptr;
    if (!r) return Status::Stopped;
    if (begin >= end) return Status::Ok;
    if (begin < 0 || end > r->fileSize) return Status::BadRequest;
    Impl::Waiting waiting(*impl, req.infoHash, reader);
    r->window.noteRead(end - begin, secondsNow());
    impl->applyWindow(*t, *r, begin);
    PieceSpan span = piecesFor(r->fileOffset, begin, end, t->pieceLength);
    span.last = std::min(span.last, int(t->have.size()) - 1);

    // stall counts from when this wait began, so a reader back from a pause isn't
    // judged by how quiet the torrent was while nobody asked for anything
    auto waitStart = Clock::now();
    auto stall = std::chrono::seconds(impl->settings.stallSecs);
    while (true) {
        if (impl->quit) return Status::Stopped;
        // look the reader up again after every wait: only its reference keeps `t` alive
        t = impl->find(req.infoHash);
        if (!t || !impl->findReader(*t, reader)) return Status::Stopped;
        if (!t->error.empty()) return Status::DiskError;
        bool ready = true;
        for (int p = span.first; p <= span.last; ++p) {
            if (!t->have[size_t(p)]) {
                ready = false;
                break;
            }
        }
        if (ready) return Status::Ok;
        if (Clock::now() - std::max(t->lastProgress, waitStart) > stall) return Status::NoPeers;
        impl->changed.wait_for(lock, std::chrono::seconds(1));
    }
}

void TorrentEngine::prefetch(const StreamRequest& req) {
    // UI thread: log what fails, never throw
    impl->guarded("prefetch", [&] {
        std::unique_lock<std::mutex> lock(impl->mutex);
        if (impl->quit) return;
        // runs on the UI thread: never wait for an idle session to finish closing
        if (impl->closing) {
            impl->log("prefetch skipped, session closing");
            return;
        }
        bool existed = impl->find(req.infoHash) != nullptr;
        std::string error;
        Torrent* t = impl->ensure(req, lock, error);
        if (!t) {
            impl->log("prefetch failed: " + error);
            return;
        }
        if (!existed) t->removeAt = Clock::now() + std::chrono::seconds(std::max(120, impl->settings.graceSecs));
    });
}

Stats TorrentEngine::stats(const std::string& infoHash) {
    Stats s;
    // UI thread: log what fails and report nothing, never throw
    bool ok = impl->guarded("stats", [&] {
        std::lock_guard<std::mutex> lock(impl->mutex);
        Torrent* t = impl->find(infoHash);
        if (!t) return;
        lt::torrent_status st = t->handle.status();
        s.peers = st.num_peers;
        s.downloadBytesPerSec = st.download_payload_rate;
        s.hasMetadata = t->metadata;
    });
    return ok ? s : Stats{};
}

void TorrentEngine::setUploadLimit(int bytesPerSec) {
    // UI thread: log what fails, never throw
    impl->guarded("setUploadLimit", [&] {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->settings.uploadLimit = bytesPerSec;
        if (impl->session) {
            lt::settings_pack sp;
            sp.set_int(lt::settings_pack::upload_rate_limit, bytesPerSec);
            impl->session->apply_settings(std::move(sp));
        }
    });
}

}  // namespace torrent
