/*
 * Clock ids on the PS4.
 *
 * Everything is compiled against musl's headers, which use Linux clock ids, but
 * clock_gettime() resolves to the kernel's FreeBSD version (musl's own one is
 * empty in OpenOrbis). Linux CLOCK_MONOTONIC is 1, and FreeBSD's 1 is
 * CLOCK_VIRTUAL: the process's user CPU time. So std::chrono::steady_clock and
 * curl's timeouts (Curl_now) ran on CPU time and barely moved while waiting on the
 * network. mpv already avoids it (mpv.patch drops _POSIX_TIMERS so it uses
 * gettimeofday).
 *
 * Linked with --wrap=clock_gettime, so every call comes through here and gets the
 * FreeBSD id.
 */
#include <errno.h>
#include <time.h>

int __real_clock_gettime(clockid_t id, struct timespec* ts);

/* FreeBSD <sys/_clock_id.h> */
#define BSD_CLOCK_REALTIME 0
#define BSD_CLOCK_MONOTONIC 4
#define BSD_CLOCK_UPTIME 5
#define BSD_CLOCK_THREAD_CPUTIME_ID 14
#define BSD_CLOCK_PROCESS_CPUTIME_ID 15

static clockid_t bsd_clock(clockid_t linux_id) {
    switch (linux_id) {
        case 0: /* CLOCK_REALTIME */
        case 5: /* CLOCK_REALTIME_COARSE */
            return BSD_CLOCK_REALTIME;
        case 1: /* CLOCK_MONOTONIC */
        case 4: /* CLOCK_MONOTONIC_RAW */
        case 6: /* CLOCK_MONOTONIC_COARSE */
            return BSD_CLOCK_MONOTONIC;
        case 7: /* CLOCK_BOOTTIME */
            return BSD_CLOCK_UPTIME;
        case 2: /* CLOCK_PROCESS_CPUTIME_ID */
            return BSD_CLOCK_PROCESS_CPUTIME_ID;
        case 3: /* CLOCK_THREAD_CPUTIME_ID */
            return BSD_CLOCK_THREAD_CPUTIME_ID;
        default:
            return -1;
    }
}

int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    clockid_t const bsd = bsd_clock(id);
    if (bsd < 0) {
        errno = EINVAL;
        return -1;
    }
    return __real_clock_gettime(bsd, ts);
}

#ifdef USE_TORRENT
#include <pthread.h>

/* Asio (libtorrent) sets CLOCK_MONOTONIC on its condition variables; same ids. */
int __real_pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t id);

int __wrap_pthread_condattr_setclock(pthread_condattr_t* attr, clockid_t id) {
    clockid_t const bsd = bsd_clock(id);
    if (bsd < 0) return EINVAL;
    return __real_pthread_condattr_setclock(attr, bsd);
}
#endif
