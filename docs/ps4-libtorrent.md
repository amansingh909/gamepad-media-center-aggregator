# libtorrent and Boost on the PS4

This fork runs [libtorrent](https://libtorrent.org) 2.0.15 inside GMCA on a jailbroken PS4 (GoldHEN),
built with the OpenOrbis toolchain. This page explains how the two libraries are built for the
console, what had to change for them to work there, and how the result is tested. Most of the
problems below aren't specific to libtorrent: any network-heavy C++ library hits them on this
toolchain.

## The two packages

Both are pacman packages, built with `makepkg` from recipes in this repo, the same way the other PS4
dependencies are.

| Package | Recipe | What it is |
|---|---|---|
| `ps4-openorbis-boost-headers` 1.83.0 | `scripts/ps4/boost-headers/` | Boost's headers only. libtorrent needs Boost.Asio and a few other header-only parts; nothing in Boost is compiled. |
| `ps4-openorbis-libtorrent` 2.0.15 | `scripts/ps4/libtorrent/` | libtorrent-rasterbar, static, with a PS4 patch and a compatibility header. |

The libtorrent recipe builds with:

- `-include ps4_compat.h` on every file (see below);
- Boost.Asio forced onto `select()`: `BOOST_ASIO_DISABLE_KQUEUE`, `BOOST_ASIO_DISABLE_EPOLL` and
  `BOOST_ASIO_DISABLE_EVENTFD`. The PS4 kernel has no epoll, and the headers don't expose kqueue;
- no OpenSSL, GnuTLS or libgcrypt. libtorrent then uses its built-in SHA-1 and RC4, so peer
  encryption still works. Only HTTPS trackers are lost; UDP and plain HTTP trackers and the DHT work;
- `deprecated-functions` and `i2p` off, static library, C++14.

Code that includes libtorrent headers (the engine in `app/src/torrent/`) must be built with the same
`-include` and defines.

### The libtorrent patch

`scripts/ps4/libtorrent/libtorrent-2.0.15-ps4.patch` adds a `TORRENT_PS4` platform to
`config.hpp`, `enum_net`, `random.cpp` and `aux_/buffer.hpp`:

- **No `mmap`, `sysctl`, `getifaddrs`, routing sockets or `getrandom`.** The console doesn't provide
  them in a usable form.
- **`ps4_set_network(ip, netmask, gateway)`.** libtorrent can't list the console's interfaces or
  routes itself, so the app reads them with `sceNetCtlGetInfo()` and hands them over. The engine
  reads them again every 30 seconds; when they change, it calls `ps4_set_network` and
  `session::reopen_network_sockets()`.
- **No `thread_local` objects with destructors in `random.cpp`.** The PS4 libc lacks
  `__cxa_thread_atexit_impl`, so the random generator uses libtorrent's mutex fallback.

### The compatibility header

OpenOrbis pairs musl's headers, which carry **Linux** values, with the PS4's **FreeBSD** kernel. Where
the two disagree, a constant compiles fine and then means something else at run time.
`scripts/ps4/libtorrent/ps4_compat.h` fixes the ones libtorrent uses:

- **`FIONBIO` and `FIONREAD`:** musl only ships the Linux numbers; the header defines the FreeBSD
  ones.
- **IP, IPv6 and TCP socket option numbers.** `SOL_SOCKET` options already use FreeBSD values, but the
  IP-level ones are Linux numbers. Linux `IP_TOS` (1) is FreeBSD `IP_OPTIONS`, so libtorrent setting
  a TOS of 0 put an IP options block on every packet it sent, and nothing ever answered. The header
  remaps them to the FreeBSD values.
- **`sa_sigaction`:** a broken macro in OpenOrbis' `struct sigaction`.

## PS4 problems fixed in the app

These are in GMCA itself, not in the packages. The first four are also sent upstream as
[thcolin/gamepad-media-center-aggregator#91](https://github.com/thcolin/gamepad-media-center-aggregator/pull/91),
because they affect every PS4 build of GMCA.

- **Initialized globals read as zero.** OpenOrbis' `link.x` collects `.data` but not `.data.*`, and
  `create-fself` doesn't load the leftover sections. On the console those globals read as zero. The
  first PS4 test of libtorrent crashed on exactly that: Boost.Asio divided by a zeroed table. CMake now links with a patched copy of `link.x`.
- **Static init across threads aborted.** OpenOrbis' libc++abi tells threads apart with `syscall(20)`,
  but `syscall()` is a stub that returns -1, so every thread looks the same. Two threads reaching the
  same function-local static made libc++abi report a recursive initialization and abort.
  `app/platform/ps4/cxa_guard.c` provides the guard functions.
- **Clocks stood still.** Everything compiles against musl's Linux clock ids, but `clock_gettime` is
  the kernel's FreeBSD one. Linux `CLOCK_MONOTONIC` (1) is FreeBSD's per-process CPU time, so
  `steady_clock` and every libtorrent timer froze. `app/platform/ps4/clock_ids.c` maps the ids, linked
  with `--wrap=clock_gettime`.
- **SIGPIPE killed the app silently.** Writing to a socket a peer had closed raised SIGPIPE.
  OpenOrbis' libc++ undefines `__FreeBSD__`, so Asio never sets `SO_NOSIGPIPE`, and musl's
  `MSG_NOSIGNAL` is Linux's 0x4000, which FreeBSD reads as `MSG_NBIO`. The app ignores SIGPIPE at
  startup. The engine's HTTP server also sets `SO_NOSIGPIPE` (FreeBSD's 0x0800, missing from the
  headers) on its sockets. Accepted sockets inherit `O_NONBLOCK` from the listener on FreeBSD, so the
  server sets each one back to blocking with `FIONBIO`.
- **`std::filesystem` doesn't work.** musl's `fstatat` is an ENOSYS stub, and `lstat` goes through it
  because `-lc` comes before `-lkernel`. `remove_all`, `directory_iterator`, `is_directory` and
  `file_size` fail or throw. The engine deletes folders with `unlink`,
  `open(O_DIRECTORY | O_NOFOLLOW)`, `opendir`/`readdir` and `rmdir`, and the Files tab lists folders
  the same way.
- **DNS.** Jailbroken consoles often use an update-blocking DNS server that resolves nothing else.
  GMCA sets musl's `primary_dns`/`secondary_dns` for itself only (`utils/config.cpp`), which covers
  the engine's tracker and DHT lookups too.

## The streaming engine

The engine lives in `app/src/torrent/` with its pure logic in header-only files in
`app/include/torrent/`. It has three parts:

- **`torrent_engine.cpp`** owns the libtorrent session: `posix_disk_io` (a finished piece can be read
  back with `pread`), the read window, piece priorities and deadlines, the DHT state saved between
  launches, optional UPnP and NAT-PMP, and diagnostics every 30 seconds.
- **`torrent_server.cpp`** serves a torrent's file over HTTP range requests to local readers, the
  same way Stremio's own server does, so mpv can play a torrent like any URL.
- **`torrent_service.cpp`** owns the engine on a worker thread. The UI never calls libtorrent directly.

Two lessons from streaming on the console:

- **Refill the window in batches.** Topped up continuously, only a few pieces are wanted and all are
  already in flight, so libtorrent stays in end-game mode and re-requests blocks other peers are
  already downloading. In the first PS4 test that tripled the traffic. The window refills when it
  drops below half.
- **Big pieces need ordering.** Movie torrents often use 8 to 16 MiB pieces, and a piece can only be
  read once all of it has arrived and been hashed. With the whole window at one priority, the swarm
  spread over about nine pieces and the player waited with 0.1 MB ready while 4 MB/s came in.
  `piece_plan.hpp` now gives the next two or three pieces top priority with deadlines and fills the
  rest of the window in file order.

Torrents are added with `auto_managed` and `paused` cleared, uploads stay limited to 1 B/s while
nothing reads a torrent, and a torrent's data is deleted shortly after its last reader leaves.

## Building and CI

To rebuild the packages locally, run `makepkg` in `scripts/ps4/boost-headers` and
`scripts/ps4/libtorrent` inside the PS4 builder image (`xfangfang/pacbrew:250221` plus the usual
OpenOrbis packages), as `.github/workflows/pacbrew.yaml` does.

GitHub's PS4 build installs the regular dependencies from upstream's `ci-deps` release, and these two
from this fork's [`ci-deps` pre-release](https://github.com/amansingh909/gamepad-media-center-aggregator/releases/tag/ci-deps)
(`TORRENT_DEPS_URL` in `.github/workflows/build.yaml`). Each file's SHA-256 is pinned there, and a
file that doesn't match isn't installed. After rebuilding either package, upload it to that
pre-release and update its hash in `build.yaml`. Keep `ci-deps` marked as a pre-release: the app's
update check reads the repository's latest release.

## Testing

- **Pure logic** (read window, piece plan, source order, resume rules and so on) has standalone tests
  in `tests/test_*.cpp`.
- **`tests/torrent/app_checks.cpp`** drives the real engine on a laptop against libtorrent 2.0.10.
  `--swarm` runs a loopback swarm with 8 and 16 MiB pieces and measures how long the reader waits;
  it fails on the old window and passes on the current one.
- **`tests/torrent/integration.sh --remote http://<ps4>:11470`** runs 19 HTTP checks against the
  engine running on a real console: range reads, reads past 4 GiB, two readers at once, a reader
  back after a pause, a dead torrent, and clients that hang up mid-transfer. The engine's server is
  only reachable from other machines in a diagnostic build (`-DTORRENT_DEBUG_LAN=ON`).
- **After every PS4 build**, every initialized `.data` section of the ELF is compared with the
  file-backed segments of the `.oelf`, so a zeroed-global problem like the first one can't come
  back unnoticed.

Results on a GoldHEN PS4:

- all integration checks pass;
- the first torrent of a session gets its metadata in about 5 seconds (28 before the DHT state was
  kept between launches);
- 1080p torrents play without stalls after the start when the swarm is healthy.
