#include <torrent/torrent_engine.hpp>

#include <torrent/pieces.hpp>
#include <torrent/read_window.hpp>

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
    std::vector<bool> deadlined;  // got a deadline since the last refill
    int pieceLength = 1;
    std::map<ReaderId, Reader> active;
};

}  // namespace detail

using detail::Reader;
using detail::Torrent;

struct TorrentEngine::Impl {
    EngineSettings settings;
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

    explicit Impl(EngineSettings s) : settings(std::move(s)) {
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

    void sweep() {
        std::error_code ec;
        fs::create_directories(settings.tempDir, ec);
        std::vector<fs::path> leftovers;
        for (auto it = fs::directory_iterator(settings.tempDir, ec); !ec && it != fs::directory_iterator();
             it.increment(ec))
            leftovers.push_back(it->path());
        for (const auto& p : leftovers) fs::remove_all(p, ec);
        if (!leftovers.empty()) log("swept " + std::to_string(leftovers.size()) + " leftovers");
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
        t.handle.set_upload_limit(1);
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

    // Moves reader `r` to `position`. Priorities change only when some reader's window
    // refills (in one batch, as the spike learned), and then cover every reader's
    // window plus its file's tail. Deadlines are topped up on every call.
    void applyWindow(Torrent& t, Reader& r, int64_t position) {
        ReadWindow::Plan plan = r.window.update(position);
        r.position = position;
        r.plan = plan;
        const int n = int(t.have.size());
        if (plan.refill) {
            r.windowStart = position;
            std::vector<lt::download_priority_t> prio(size_t(n), lt::dont_download);
            auto mark = [&](PieceSpan s) {
                for (int p = std::max(0, s.first); p <= s.last && p < n; ++p) prio[size_t(p)] = lt::top_priority;
            };
            for (const auto& kv : t.active) {
                const Reader& o = kv.second;
                // [windowStart, wantEnd), not [position, wantEnd): a jump back inside the
                // reader's window doesn't refill it, so those pieces must stay wanted
                if (o.plan.wantEnd > o.windowStart)
                    mark(piecesFor(o.fileOffset, o.windowStart, o.plan.wantEnd, t.pieceLength));
                // MP4 files often keep their index at the end, and players read it first
                mark(piecesFor(o.fileOffset, std::max<int64_t>(0, o.fileSize - (2 << 20)), o.fileSize,
                               t.pieceLength));
            }
            // clear first: libtorrent sets every piece whose deadline it clears to low
            // priority, which would override the priorities below
            t.handle.clear_piece_deadlines();
            t.handle.prioritize_pieces(prio);
            std::fill(t.deadlined.begin(), t.deadlined.end(), false);
        }
        for (const auto& kv : t.active) {
            const Reader& o = kv.second;
            if (o.plan.deadlineEnd <= o.position) continue;
            PieceSpan urgent = piecesFor(o.fileOffset, o.position, o.plan.deadlineEnd, t.pieceLength);
            double rate = std::max(o.window.rate(), double(1 << 20));
            for (int p = std::max(0, urgent.first); p <= urgent.last && p < n; ++p) {
                if (t.have[size_t(p)] || t.deadlined[size_t(p)]) continue;
                int64_t ahead = std::max<int64_t>(0, int64_t(p) * t.pieceLength - o.fileOffset - o.position);
                t.handle.set_piece_deadline(lt::piece_index_t(p), int(double(ahead) / rate * 1000));
                t.deadlined[size_t(p)] = true;
            }
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
                std::error_code ec;
                fs::remove_all(it->first, ec);
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

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!quit) {
            lt::session* s = session.get();
            if (!s) {
                changed.wait_for(lock, std::chrono::milliseconds(250));
                expire(lock);
                continue;
            }
            // only this thread destroys the session, so `s` stays valid unlocked
            lock.unlock();
            s->wait_for_alert(lt::milliseconds(250));
            std::vector<lt::alert*> alerts;
            s->pop_alerts(&alerts);
            lock.lock();
            for (lt::alert* a : alerts) handle(a);
            if (!alerts.empty()) changed.notify_all();
            expire(lock);
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
    // a reader reference keeps `t` in the map (and its address valid) while we wait
    // lift dropReader()'s 1 B/s throttle: the session-wide cap governs while someone reads
    if (t->readers++ == 0) t->handle.set_upload_limit(-1);
    t->removeAt = Clock::time_point::max();
    auto giveUp = [&](Status s) {
        impl->dropReader(*t);
        return s;
    };

    auto deadline = Clock::now() + std::chrono::seconds(impl->settings.stallSecs);
    while (!t->metadata && t->error.empty() && !impl->quit) {
        if (impl->changed.wait_until(lock, deadline) == std::cv_status::timeout && !t->metadata)
            return giveUp(Status::NoPeers);
    }
    if (impl->quit) return giveUp(Status::Stopped);
    if (!t->error.empty()) return giveUp(Status::DiskError);

    const lt::file_storage& files = t->info->files();
    std::vector<int64_t> sizes;
    for (int i = 0; i < files.num_files(); ++i) sizes.push_back(files.file_size(lt::file_index_t(i)));
    int index = pickFile(req.fileIndex, sizes);
    if (index < 0) return giveUp(Status::NotFound);

    lt::file_index_t fi(index);
    ReaderId id = impl->nextReader++;
    t->active.emplace(std::piecewise_construct, std::forward_as_tuple(id),
                      std::forward_as_tuple(files.file_offset(fi), files.file_size(fi)));
    reader = id;
    file.name = files.file_path(fi);
    file.diskPath = files.file_path(fi, t->savePath);
    file.size = files.file_size(fi);
    return Status::Ok;
}

void TorrentEngine::release(const StreamRequest& req, ReaderId reader) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    Torrent* t = impl->find(req.infoHash);
    if (!t || !impl->findReader(*t, reader)) return;
    t->active.erase(reader);
    impl->dropReader(*t);
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
}

Stats TorrentEngine::stats(const std::string& infoHash) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    Stats s;
    Torrent* t = impl->find(infoHash);
    if (!t) return s;
    lt::torrent_status st = t->handle.status();
    s.peers = st.num_peers;
    s.downloadBytesPerSec = st.download_payload_rate;
    s.hasMetadata = t->metadata;
    return s;
}

void TorrentEngine::setUploadLimit(int bytesPerSec) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->settings.uploadLimit = bytesPerSec;
    if (impl->session) {
        lt::settings_pack sp;
        sp.set_int(lt::settings_pack::upload_rate_limit, bytesPerSec);
        impl->session->apply_settings(std::move(sp));
    }
}

}  // namespace torrent
