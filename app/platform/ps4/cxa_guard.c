/*
 * Thread-safe static initialization on the PS4.
 *
 * OpenOrbis' libc++abi tells threads apart with syscall(20), but syscall() in
 * OpenOrbis' libc is a stub that sets errno to ENOSYS and returns -1, so every
 * thread gets the same id. When two threads reach the same function-local static
 * at once (ThreadPool workers creating their first HTTP, whose static Global runs
 * curl_global_init), libc++abi takes the second thread for a recursive
 * initialization and aborts.
 *
 * This file provides the Itanium ABI guard functions instead, so the linker leaves
 * libc++abi's cxa_guard.o out. Byte 0 of the guard is the "done" flag the compiler
 * checks inline; byte 1 marks an initialization in progress. A thread that finds
 * one in progress waits for it, whichever thread it is.
 */
#include <pthread.h>
#include <stdint.h>

static pthread_mutex_t guard_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t guard_cond = PTHREAD_COND_INITIALIZER;

int __cxa_guard_acquire(uint64_t* guard) {
    uint8_t* g = (uint8_t*)guard;
    if (__atomic_load_n(&g[0], __ATOMIC_ACQUIRE)) return 0;

    pthread_mutex_lock(&guard_mutex);
    while (g[1]) pthread_cond_wait(&guard_cond, &guard_mutex);
    int const must_init = !__atomic_load_n(&g[0], __ATOMIC_RELAXED);
    if (must_init) g[1] = 1;
    pthread_mutex_unlock(&guard_mutex);
    return must_init;
}

void __cxa_guard_release(uint64_t* guard) {
    uint8_t* g = (uint8_t*)guard;
    pthread_mutex_lock(&guard_mutex);
    __atomic_store_n(&g[0], 1, __ATOMIC_RELEASE);
    g[1] = 0;
    pthread_cond_broadcast(&guard_cond);
    pthread_mutex_unlock(&guard_mutex);
}

void __cxa_guard_abort(uint64_t* guard) {
    uint8_t* g = (uint8_t*)guard;
    pthread_mutex_lock(&guard_mutex);
    g[1] = 0;
    pthread_cond_broadcast(&guard_cond);
    pthread_mutex_unlock(&guard_mutex);
}
