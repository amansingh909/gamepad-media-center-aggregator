#include <torrent/torrent_engine.hpp>

#include <torrent/piece_plan.hpp>
#include <torrent/pieces.hpp>
#include <torrent/posix_tree.hpp>
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
#include <torrent/network_watch.hpp>

#include <cstring>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
static_assert(lt::default_block_size == blockBytes, "pieces.hpp's block is libtorrent's");

namespace {

double secondsNow() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// A prefetched torrent only needs its metadata and a few peers; its first reader lifts this.
const int prefetchConnections = 20;
// Prefetched torrents nobody has opened yet: past this many, the oldest goes.
const size_t maxPrefetched = 3;
// How long a prefetch keeps a torrent no reader has: a new one, or one still here after its
// last reader (a replay soon after stopping then starts at once).
const int prefetchKeepSecs = 120;

// "Not enough storage space" is only for a full disk or quota; any other file error means
// the source can't be played. (OpenOrbis' errno.h has the FreeBSD values the PS4 returns.)
bool isDiskFull(const lt::error_code& ec) {
    if (ec.category() != lt::system_category() && ec.category() != lt::generic_category()) return false;
    return ec.value() == ENOSPC || ec.value() == EDQUOT;
}

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
    std::string error;      // set by the first file or torrent error alert
    bool diskFull = false;  // that error is a full disk or quota
    bool prefetchOnly = false;  // added by prefetch(), no reader yet
    Clock::time_point addedAt = Clock::now();
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
    Clock::time_point nextStats = Clock::time_point::max();  // the next stats lines, while a session exists
    bool quit = false;
    bool closing = false;  // expire() is destroying an idle session with the lock dropped
    bool keepOpen = false;  // keepSessionOpen(true): the session doesn't close for idleness
    Clock::time_point nextStateSave = Clock::time_point::max();  // the next DHT state save, while a session exists
#ifdef __PS4__
    NetworkWatch network;  // the console's network as libtorrent was last told it
    Clock::time_point nextNetworkCheck = Clock::time_point::max();  // the next NetCtl reading, while a session exists
#endif
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
        if (s) saveState(*s);
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
    // POSIX calls only: std::filesystem::remove_all fails with ENOSYS on the PS4.
    bool removeFolder(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        if (!isEngineFolderName(slash == std::string::npos ? path : path.substr(slash + 1))) {
            log("not deleting " + path + ": not an engine folder");
            return false;
        }
        std::string error;
        const bool ok = removeTree(path, error);
        if (!ok) log("deleting " + path + ": " + error);
        return ok;
    }

    // Deletes what a crash or an earlier run left in tempDir. Only the engine's own
    // folders: anything else in there is left alone.
    void sweep() {
        if (!tempOk) return;
        std::error_code ec;
        fs::create_directories(settings.tempDir, ec);
        if (ec) log("creating " + settings.tempDir + ": " + ec.message());
        std::vector<std::string> names;
        std::string error;
        if (!listFolder(settings.tempDir, names, error)) log("listing " + error);
        int removed = 0;
        for (const auto& name : names)
            if (isEngineFolderName(name) && removeFolder(settings.tempDir + "/" + name)) ++removed;
        if (removed > 0) log("swept " + std::to_string(removed) + " leftovers");
    }

#ifdef __PS4__
    // libtorrent can't list interfaces or routes on the PS4 (the TORRENT_PS4 patch), so hand
    // it what NetCtl reports: before a session starts, and every 30 s while one is open, since
    // it stays open for the app's life (a cable plugged in, another Wi-Fi, a new DHCP lease).
    // New addresses, or the network back after a failed reading, reopen a running session's
    // sockets. Engine lock held; borealis has already called sceNetCtlInit.
    void followPs4Network() {
        auto text = [](const char* field, size_t size) { return std::string(field, strnlen(field, size)); };
        OrbisNetCtlInfo ip{}, mask{}, gw{};
        NetworkReading r;
        lt::address a, m, g;
        std::string why;
        if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &ip) < 0 ||
            sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_NETMASK, &mask) < 0 ||
            sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_DEFAULT_ROUTE, &gw) < 0) {
            why = "no network info from NetCtl";
        } else {
            r.ip = text(ip.ip_address, sizeof ip.ip_address);
            r.netmask = text(mask.netmask, sizeof mask.netmask);
            r.gateway = text(gw.default_route, sizeof gw.default_route);
            lt::error_code e1, e2, e3;
            a = lt::make_address(r.ip, e1);
            m = lt::make_address(r.netmask, e2);
            g = lt::make_address(r.gateway, e3);
            r.ok = !e1 && !e2 && !e3;
            if (!r.ok) why = "unparsable NetCtl addresses";
        }
        switch (network.update(r)) {
            case NetworkWatch::Action::None: break;
            case NetworkWatch::Action::Failed: log(why); break;
            case NetworkWatch::Action::Changed:
                lt::ps4_set_network(a, m, g);
                if (session) {
                    session->reopen_network_sockets();
                    log("network changed: " + r.ip);
                }
                break;
        }
    }
#endif

    void startSession() {
#ifdef __PS4__
        followPs4Network();
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
        // the addon's trackers and the default ones at once, not one tier after another
        sp.set_bool(lt::settings_pack::announce_to_all_tiers, true);
        sp.set_bool(lt::settings_pack::announce_to_all_trackers, true);
        lt::session_params params(std::move(sp));
        loadState(params);
        // The server relies on this: once waitFor() returns Ok, pread() of the file sees
        // the verified bytes. The POSIX backend writes each block to the file before
        // completing it and hashes a piece by reading it back from the file, so a piece
        // that passed is on disk. The default mmap backend can hash queued blocks from
        // memory before they reach the file. It is also the PS4 build's backend.
        params.disk_io_constructor = lt::posix_disk_io_constructor;
        session = std::make_unique<lt::session>(std::move(params));
        idleSince = Clock::now();
        nextStats = Clock::now() + std::chrono::seconds(30);
        nextStateSave = Clock::now() + std::chrono::minutes(2);  // once the DHT has nodes
#ifdef __PS4__
        nextNetworkCheck = Clock::now() + std::chrono::seconds(30);
#endif
        log("session started");
    }

    // The DHT nodes the last session knew: a new session asks them first instead of
    // bootstrapping from nothing (the console's first torrent waited 28 s for its peers). No
    // file is a first run; a damaged one is logged and ignored, and the next save (once the DHT
    // has nodes) replaces it.
    void loadState(lt::session_params& params) {
        if (settings.stateFile.empty()) return;
        std::FILE* f = std::fopen(settings.stateFile.c_str(), "rb");
        if (!f) return;
        std::vector<char> buf;
        char chunk[4096];
        size_t n;
        while (buf.size() < (size_t(4) << 20) && (n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
            buf.insert(buf.end(), chunk, chunk + n);
        std::fclose(f);
        try {
            lt::span<char const> bytes(buf.data(), std::ptrdiff_t(buf.size()));
            lt::session_params saved = lt::read_session_params(bytes, lt::session::save_dht_state);
            params.dht_state = std::move(saved.dht_state);
            log("loaded the DHT state: " +
                std::to_string(params.dht_state.nodes.size() + params.dht_state.nodes6.size()) + " nodes");
        } catch (const std::exception& e) {
            log(std::string("DHT state unreadable, starting without it: ") + e.what());
        }
    }

    // Writes the DHT state for the next session: a temp file renamed over the old one, so a
    // crash mid-write leaves the last good copy. A DHT that knows no node yet (a session that
    // closes before it bootstraps) leaves the file as it is. Never throws.
    void saveState(lt::session& s) {
        if (settings.stateFile.empty()) return;
        guarded("saving the DHT state", [&] {
            const lt::session_params p = s.session_state(lt::session::save_dht_state);
            if (p.dht_state.nodes.empty() && p.dht_state.nodes6.empty()) {
                log("DHT state not saved: no nodes");
                return;
            }
            const std::vector<char> buf = lt::write_session_params_buf(p, lt::session::save_dht_state);
            const std::string tmp = settings.stateFile + ".part";
            std::FILE* f = std::fopen(tmp.c_str(), "wb");
            if (!f) {
                log("saving the DHT state: cannot write " + tmp);
                return;
            }
            const bool written = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
            const bool closed = std::fclose(f) == 0;
            if (!written || !closed || std::rename(tmp.c_str(), settings.stateFile.c_str()) != 0) {
                std::remove(tmp.c_str());
                log("saving the DHT state: write failed");
                return;
            }
            log("saved the DHT state: " + std::to_string(p.dht_state.nodes.size() + p.dht_state.nodes6.size()) +
                " nodes");
        });
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

    Torrent* ensure(const StreamRequest& req, std::unique_lock<std::mutex>& lock, std::string& error,
                    bool forPrefetch = false) {
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
        // an uppercase hash would name a folder the sweep never deletes
        if (!isInfoHash(req.infoHash)) {
            error = "bad info hash \"" + req.infoHash + "\"";
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
        // the default trackers after the addon's, each in a tier of its own like the
        // magnet's (startSession() announces to every tier at once)
        int tier = atp.tracker_tiers.empty() ? 0 : atp.tracker_tiers.back() + 1;
        atp.tracker_tiers.resize(atp.trackers.size(), 0);
        for (const auto& tracker : settings.defaultTrackers) {
            if (std::find(atp.trackers.begin(), atp.trackers.end(), tracker) != atp.trackers.end()) continue;
            atp.trackers.push_back(tracker);
            atp.tracker_tiers.push_back(tier++);
        }
        if (forPrefetch) atp.max_connections = prefetchConnections;
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

    // A torrent error as the caller sees it: only a full disk is "Not enough storage space".
    static Status failure(const Torrent& t) { return t.diskFull ? Status::DiskError : Status::Failed; }

    // Past maxPrefetched torrents that only a prefetch added, the oldest goes now (expire()
    // removes it within a quarter second): scrolling a long source list mustn't keep a
    // dozen swarms open.
    void dropOldPrefetches() {
        std::vector<Torrent*> waiting;
        for (auto& kv : torrents)
            if (kv.second.prefetchOnly && kv.second.readers == 0) waiting.push_back(&kv.second);
        if (waiting.size() <= maxPrefetched) return;
        std::sort(waiting.begin(), waiting.end(),
                  [](const Torrent* a, const Torrent* b) { return a->addedAt < b->addedAt; });
        for (size_t i = 0; i + maxPrefetched < waiting.size(); ++i) waiting[i]->removeAt = Clock::now();
    }

    // The first error decides; a full disk repeats it for every block.
    void onError(Torrent& t, const lt::error_code& ec, const std::string& message, const char* what) {
        if (!t.error.empty()) return;
        t.error = message;
        t.diskFull = isDiskFull(ec);
        log(std::string(what) + ": " + message);
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
            if (Torrent* t = byHandle(fe->handle)) onError(*t, fe->error, fe->message(), "file error");
        } else if (auto* te = lt::alert_cast<lt::torrent_error_alert>(a)) {
            if (Torrent* t = byHandle(te->handle)) onError(*t, te->error, te->message(), "torrent error");
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
        std::vector<std::string> due;
        for (auto it = pendingDirs.begin(); it != pendingDirs.end();) {
            if (now >= it->second) {
                due.push_back(it->first);
                it = pendingDirs.erase(it);
            } else {
                ++it;
            }
        }
        if (!due.empty()) {
            // a multi-gigabyte folder takes a while: waiters and stats() don't wait for it
            lock.unlock();
            for (const auto& dir : due) removeFolder(dir);
            lock.lock();
        }
        if (!torrents.empty() || !session || keepOpen) {
            idleSince = now;
            return;
        }
        if (now - idleSince > std::chrono::seconds(settings.idleSecs)) {
            std::unique_ptr<lt::session> s = std::move(session);
            log("idle, closing the session");
            closing = true;
            nextStateSave = Clock::time_point::max();
            lock.unlock();
            saveState(*s);
            s.reset();
            lock.lock();
            closing = false;
            changed.notify_all();
        }
    }

    // Every 30 s, how each torrent is doing: torrent.log is all a console run leaves
    // behind (uploads during the grace period, bytes downloaded twice or thrown away).
    void logStats() {
        auto now = Clock::now();
        if (!session || now < nextStats) return;
        nextStats = now + std::chrono::seconds(30);
        auto mb = [](int64_t bytes) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", double(bytes) / (1 << 20));
            return std::string(buf);
        };
        for (auto& kv : torrents) {
            // no query flags: these fields are always filled in
            lt::torrent_status st = kv.second.handle.status(lt::status_flags_t{});
            log(kv.first + ": peers " + std::to_string(st.num_peers) + ", down " +
                std::to_string(st.download_payload_rate / 1024) + " KB/s, up " +
                std::to_string(st.upload_payload_rate / 1024) + " KB/s, redundant " + mb(st.total_redundant_bytes) +
                " MB, failed " + mb(st.total_failed_bytes) + " MB, readers " + std::to_string(kv.second.readers));
        }
    }

    // One round of the worker: alerts, stats, expiry.
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
        logStats();
#ifdef __PS4__
        if (Clock::now() >= nextNetworkCheck) {
            nextNetworkCheck = Clock::now() + std::chrono::seconds(30);
            followPs4Network();
        }
#endif
        if (Clock::now() >= nextStateSave) {
            // a PS-button close never reaches the destructor: keep a recent copy on disk
            nextStateSave = Clock::now() + std::chrono::minutes(15);
            lock.unlock();
            saveState(*s);
            lock.lock();
        }
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

Status TorrentEngine::acquire(const StreamRequest& req, FileInfo& file, ReaderId& reader,
                              const std::function<bool()>& abandoned) {
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
    // and a prefetch's connection cap
    if (t->prefetchOnly) {
        t->prefetchOnly = false;
        t->handle.set_max_connections(-1);
    }
    t->removeAt = Clock::time_point::max();

    auto deadline = Clock::now() + std::chrono::seconds(impl->settings.stallSecs);
    while (!t->metadata && t->error.empty() && !impl->quit) {
        if (abandoned && abandoned()) return Status::Stopped;
        auto now = Clock::now();
        if (now >= deadline) return Status::NoPeers;
        // wake at least once a second to ask `abandoned` again
        impl->changed.wait_until(lock, std::min(deadline, now + std::chrono::seconds(1)));
    }
    if (impl->quit) return Status::Stopped;
    if (!t->error.empty()) return Impl::failure(*t);

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

Status TorrentEngine::waitFor(const StreamRequest& req, ReaderId reader, int64_t begin, int64_t end,
                              const std::function<bool()>& abandoned) {
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
        if (!t->error.empty()) return Impl::failure(*t);
        bool ready = true;
        for (int p = span.first; p <= span.last; ++p) {
            if (!t->have[size_t(p)]) {
                ready = false;
                break;
            }
        }
        if (ready) return Status::Ok;
        // a client that went away doesn't keep its reader (and its window) waiting: with
        // another reader making progress, the stall timer alone would never let it go
        if (abandoned && abandoned()) return Status::Stopped;
        if (Clock::now() - std::max(t->lastProgress, waitStart) > stall) return Status::NoPeers;
        impl->changed.wait_for(lock, std::chrono::seconds(1));
    }
}

void TorrentEngine::prefetch(const StreamRequest& req) {
    // log what fails, never throw
    impl->guarded("prefetch", [&] {
        std::unique_lock<std::mutex> lock(impl->mutex);
        if (impl->quit) return;
        // never wait for an idle session to finish closing
        if (impl->closing) {
            impl->log("prefetch skipped, session closing");
            return;
        }
        bool existed = impl->find(req.infoHash) != nullptr;
        std::string error;
        Torrent* t = impl->ensure(req, lock, error, true);
        if (!t) {
            impl->log("prefetch failed: " + error);
            return;
        }
        const int keepSecs = std::max(prefetchKeepSecs, impl->settings.graceSecs);
        const auto keepUntil = Clock::now() + std::chrono::seconds(keepSecs);
        if (existed) {
            // in its grace period or prefetched before: kept for a prefetch's time from now
            if (t->readers == 0) {
                t->removeAt = std::max(t->removeAt, keepUntil);
                if (t->prefetchOnly) t->addedAt = Clock::now();  // the latest looked at goes last
                impl->log("prefetch " + req.infoHash + ": kept " + std::to_string(keepSecs) + " s");
            }
            return;
        }
        t->prefetchOnly = true;
        t->removeAt = keepUntil;
        impl->dropOldPrefetches();
    });
}

Stats TorrentEngine::stats(const std::string& infoHash) {
    Stats s;
    // log what fails and report nothing, never throw
    bool ok = impl->guarded("stats", [&] {
        std::lock_guard<std::mutex> lock(impl->mutex);
        Torrent* t = impl->find(infoHash);
        if (!t) return;
        // no query flags: peers and rates are always filled in, the piece bitfield isn't needed
        lt::torrent_status st = t->handle.status(lt::status_flags_t{});
        s.known = true;
        s.hasMetadata = t->metadata;
        s.peers = st.num_peers;
        s.downloadBytesPerSec = st.download_payload_rate;
        if (!t->error.empty()) s.error = Impl::failure(*t);
    });
    return ok ? s : Stats{};
}

int64_t TorrentEngine::bytesReady(const StreamRequest& req, ReaderId reader, int64_t begin, int64_t end) {
    int64_t ready = -1;
    // log what fails and report nothing, never throw
    impl->guarded("bytesReady", [&] {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->quit) return;
        Torrent* t = impl->find(req.infoHash);
        Reader* r = t ? impl->findReader(*t, reader) : nullptr;
        if (!r || !t->metadata || !t->info) return;
        begin = std::max<int64_t>(0, begin);
        end = std::min(end, r->fileSize);
        if (end <= begin) {
            ready = 0;
            return;
        }
        PieceSpan span = piecesFor(r->fileOffset, begin, end, t->pieceLength);
        bool whole = true;
        for (int p = span.first; p <= span.last && whole; ++p) whole = size_t(p) < t->have.size() && t->have[size_t(p)];
        std::vector<PartialPiece> partial;
        if (!whole) {
            // the blocks arrays belong to the session until its next get_download_queue(),
            // which only this call makes, under the engine's lock
            std::vector<lt::partial_piece_info> queue;
            t->handle.get_download_queue(queue);
            for (const lt::partial_piece_info& q : queue) {
                const int p = static_cast<int>(q.piece_index);
                if (p < span.first || p > span.last || !q.blocks) continue;
                PartialPiece pp;
                pp.piece = p;
                pp.written.resize(size_t(std::max(0, q.blocks_in_piece)));
                for (int b = 0; b < q.blocks_in_piece; ++b) {
                    const auto state = q.blocks[b].state;
                    pp.written[size_t(b)] = state == lt::block_info::writing || state == lt::block_info::finished;
                }
                partial.push_back(std::move(pp));
            }
        }
        ready = bytesIn(r->fileOffset, begin, end, t->pieceLength, t->info->total_size(), t->have, partial);
    });
    return ready;
}

void TorrentEngine::setUploadLimit(int bytesPerSec) {
    // log what fails, never throw
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

void TorrentEngine::keepSessionOpen(bool on) {
    // log what fails, never throw
    impl->guarded("keepSessionOpen", [&] {
        std::unique_lock<std::mutex> lock(impl->mutex);
        if (impl->quit || impl->keepOpen == on) return;
        impl->keepOpen = on;
        impl->log(on ? "keeping the session open" : "the session closes when idle");
        if (!on) return;
        // a session closing for idleness still holds the port: let it go first
        impl->changed.wait(lock, [this] { return !impl->closing || impl->quit; });
        if (!impl->quit && !impl->session) impl->startSession();
    });
}

}  // namespace torrent
