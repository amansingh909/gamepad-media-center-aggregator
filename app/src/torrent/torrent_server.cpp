#include <torrent/torrent_server.hpp>

#include <torrent/byte_range.hpp>
#include <torrent/http_head.hpp>
#include <torrent/stream_url.hpp>
#include <torrent/torrent_engine.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <iterator>
#include <list>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __PS4__
#ifndef SO_NOSIGPIPE
#define SO_NOSIGPIPE 0x0800  // FreeBSD value; OpenOrbis' headers omit it
#endif
#endif

namespace torrent {

namespace {

const int64_t chunkSize = 256 << 10;

int httpCodeFor(Status s) {
    switch (s) {
        case Status::Ok: return 200;
        case Status::BadRequest: return 400;
        case Status::NotFound: return 404;
        case Status::NoPeers: return 504;
        case Status::DiskError: return 507;
        case Status::Stopped: return 503;
    }
    return 500;
}

// what the user eventually reads (spec, failure handling)
std::string messageFor(Status s) {
    switch (s) {
        case Status::NoPeers: return "No peers found for this source, try another one";
        case Status::DiskError: return "Not enough storage space";
        case Status::BadRequest:
        case Status::NotFound: return "This source can't be played";
        default: return "Torrent engine stopped";
    }
}

bool sendAll(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::send(fd, data, len, 0);  // SIGPIPE is ignored (and SO_NOSIGPIPE on PS4)
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        len -= size_t(n);
    }
    return true;
}

bool readHead(int fd, std::string& head) {
    char buf[1024];
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > 16 * 1024) return false;
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        head.append(buf, size_t(n));
    }
    return true;
}

void sendError(int fd, int code, const std::string& text, const std::string& extraHeaders = "") {
    std::string body = text + "\n";
    std::string r = statusLine(code) + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + body;
    sendAll(fd, r.data(), r.size());
}

}  // namespace

struct TorrentServer::Impl {
    // One accepted connection. Its thread closes `fd` and sets `done` under connMutex,
    // so stop() never shuts down an fd number that was closed and reused.
    struct Conn {
        std::thread thread;
        int fd = -1;
        bool done = false;
    };

    TorrentEngine& engine;
    std::function<void(const std::string&)> logFn;
    int listenFd = -1;
    std::atomic<bool> stopping{false};
    std::thread acceptThread;
    std::mutex connMutex;
    std::list<Conn> conns;  // a list: each thread holds a reference to its own entry

    Impl(TorrentEngine& e, std::function<void(const std::string&)> l) : engine(e), logFn(std::move(l)) {}

    void log(const std::string& m) {
        if (logFn) logFn(m);
    }

    // an engine error before the response headers: the client gets it as the status
    void sendStatus(int fd, const StreamRequest& stream, Status st) {
        log(stream.infoHash + ": " + statusName(st));
        sendError(fd, httpCodeFor(st), messageFor(st));
    }

    void serve(int fd) {
        timeval tv{10, 0};  // a client that never sends its request
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string head;
        RequestHead req;
        if (!readHead(fd, head) || !parseRequestHead(head, req)) return sendError(fd, 400, "bad request");
        if (req.method != "GET" && req.method != "HEAD") return sendError(fd, 405, "only GET and HEAD");
        StreamRequest stream;
        if (!parseStreamPath(req.target, stream)) return sendError(fd, 400, "expected /<info hash>/<file index>");

        // one connection is one reader
        FileInfo file;
        ReaderId reader = 0;
        Status st = engine.acquire(stream, file, reader);
        if (st != Status::Ok) return sendStatus(fd, stream, st);
        struct Release {
            TorrentEngine& engine;
            const StreamRequest& stream;
            ReaderId reader;
            ~Release() { engine.release(stream, reader); }
        } release{engine, stream, reader};

        ByteRange range;
        RangeKind kind = parseRange(req.range, file.size, range);
        if (kind == RangeKind::Unsatisfiable)
            return sendError(fd, 416, "range not satisfiable", "Content-Range: bytes */" + std::to_string(file.size) + "\r\n");
        bool headOnly = req.method == "HEAD";
        int64_t length = file.size == 0 ? 0 : range.last - range.first + 1;

        // wait for the first bytes before answering, so a dead torrent gets a real error
        if (!headOnly && length > 0) {
            st = engine.waitFor(stream, reader, range.first, std::min(range.last + 1, range.first + chunkSize));
            if (st != Status::Ok) return sendStatus(fd, stream, st);
        }

        std::string h = statusLine(kind == RangeKind::Partial ? 206 : 200) +
                        "\r\nContent-Type: " + contentTypeFor(file.name) +
                        "\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(length) + "\r\n";
        if (kind == RangeKind::Partial)
            h += "Content-Range: bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" +
                 std::to_string(file.size) + "\r\n";
        h += "Connection: close\r\n\r\n";
        if (!sendAll(fd, h.data(), h.size()) || headOnly || length == 0) return;

        int fileFd = ::open(file.diskPath.c_str(), O_RDONLY);
        if (fileFd < 0) {
            log("cannot open " + file.diskPath + ": " + std::strerror(errno));
            return;
        }
        struct CloseFile {
            int fd;
            ~CloseFile() { ::close(fd); }
        } closeFile{fileFd};
        std::vector<char> buf(static_cast<size_t>(chunkSize));
        for (int64_t pos = range.first; pos <= range.last && !stopping;) {
            int64_t end = std::min(range.last + 1, pos + chunkSize);
            st = engine.waitFor(stream, reader, pos, end);
            if (st != Status::Ok) {
                // torrent.log is the only place the console tells us why a stream stopped
                log("stream ended at " + std::to_string(pos) + ": " + statusName(st));
                break;
            }
            ssize_t n = ::pread(fileFd, buf.data(), size_t(end - pos), off_t(pos));
            if (n <= 0) {
                log("read failed at " + std::to_string(pos));
                break;
            }
            if (!sendAll(fd, buf.data(), size_t(n))) break;  // the client went away
            pos += n;
        }
    }

    // joins the connections that have finished; only the accept thread calls this
    void reap() {
        std::list<Conn> finished;
        {
            std::lock_guard<std::mutex> lock(connMutex);
            for (auto it = conns.begin(); it != conns.end();) {
                auto next = std::next(it);
                if (it->done) finished.splice(finished.end(), conns, it);
                it = next;
            }
        }
        for (Conn& c : finished) c.thread.join();
    }

    void acceptLoop() {
        while (!stopping) {
            reap();
            pollfd pfd{listenFd, POLLIN, 0};
            if (::poll(&pfd, 1, 500) <= 0) continue;
            int fd = ::accept(listenFd, nullptr, nullptr);
            if (fd < 0) {
                // the client went away between poll() and accept(): nothing to accept
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR) continue;
                log(std::string("accept: ") + std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            // BSD (the PS4 kernel) gives the accepted socket the listening socket's
            // non-blocking flag, Linux doesn't: sendAll() needs a blocking socket
            int off = 0;
            if (::ioctl(fd, FIONBIO, &off) != 0) {
                log(std::string("blocking connection socket: ") + std::strerror(errno));
                ::close(fd);
                continue;
            }
#ifdef __PS4__
            // a second guard beside the ignored signal, which is unproven on the console
            // (OpenOrbis' signal headers use Linux layouts on a FreeBSD kernel)
            int noSigpipe = 1;
            if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof noSigpipe) != 0)
                log(std::string("SO_NOSIGPIPE: ") + std::strerror(errno));
#endif
            std::lock_guard<std::mutex> lock(connMutex);
            conns.emplace_back();
            Conn& c = conns.back();
            c.fd = fd;
            try {
                c.thread = std::thread([this, &c, fd] {
                    // an exception escaping a thread ends the app; serve()'s guards still
                    // release the reader and close the file while it unwinds
                    try {
                        serve(fd);
                    } catch (const std::exception& e) {
                        log(std::string("connection error: ") + e.what());
                    } catch (...) {
                        log("connection error");
                    }
                    std::lock_guard<std::mutex> lock(connMutex);
                    ::close(fd);
                    c.done = true;
                });
            } catch (const std::system_error& e) {
                log(std::string("no thread for a connection: ") + e.what());
                ::close(fd);
                conns.pop_back();
            }
        }
    }
};

TorrentServer::TorrentServer(TorrentEngine& engine, std::function<void(const std::string&)> log)
    : impl(std::make_unique<Impl>(engine, std::move(log))) {}

TorrentServer::~TorrentServer() { stop(); }

bool TorrentServer::start(uint16_t port, bool lan, std::string& error) {
    // a client hanging up mid-send must not kill the app
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) impl->log(std::string("ignoring SIGPIPE: ") + std::strerror(errno));
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
#ifdef __PS4__
    addr.sin_len = sizeof addr;
#endif
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, 16) != 0) {
        error = "port " + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    // non-blocking, so a client that resets between poll() and accept() can't leave
    // accept() blocked (on BSD) and stop() waiting on the accept thread
    int on = 1;
    if (::ioctl(fd, FIONBIO, &on) != 0) {
        error = std::string("non-blocking listening socket: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    impl->listenFd = fd;
    impl->stopping = false;
    impl->acceptThread = std::thread([this] { impl->acceptLoop(); });
    impl->log(std::string("serving on ") + (lan ? "every interface" : "127.0.0.1") + ", port " + std::to_string(port));
    return true;
}

void TorrentServer::stop() {
    if (!impl->acceptThread.joinable()) return;
    impl->stopping = true;
    impl->acceptThread.join();  // from here on, nothing adds to or reaps `conns`
    ::close(impl->listenFd);
    impl->listenFd = -1;
    // waiters in acquire() and waitFor() return Stopped
    impl->engine.shutdown();
    {
        // a blocked recv() of the request head or send() to a client that stopped
        // reading returns; one that hasn't blocked yet fails at once
        std::lock_guard<std::mutex> lock(impl->connMutex);
        for (Impl::Conn& c : impl->conns)
            if (!c.done) ::shutdown(c.fd, SHUT_RDWR);
    }
    // unlocked: each thread takes connMutex to finish
    for (Impl::Conn& c : impl->conns) c.thread.join();
    impl->conns.clear();
}

}  // namespace torrent
