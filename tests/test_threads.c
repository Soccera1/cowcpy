#define _POSIX_C_SOURCE 200809L
#include "cowcpy.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef COWCPY_HAVE_CONFIG
#include "cowcpy_config.h"
#endif
#ifndef COWCPY_SIGNAL_DIRECT
#define COWCPY_SIGNAL_DIRECT 0
#endif

#define THREADS 8
#define ROUNDS 40
#define PAGES 12

static size_t page;
static pthread_mutex_t barrier_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_changed = PTHREAD_COND_INITIALIZER;
static unsigned arrivals;
static unsigned generation;
static unsigned participants;
static unsigned char *shared_src;
static unsigned char *shared_dst;
static atomic_int stop_materializing;

/* pthread_barrier_t is optional and absent on some non-Linux targets. */
static void barrier(void)
{
    unsigned observed;
    assert(pthread_mutex_lock(&barrier_mutex) == 0);
    observed = generation;
    if (++arrivals == participants) {
        arrivals = 0;
        ++generation;
        assert(pthread_cond_broadcast(&barrier_changed) == 0);
    } else {
        while (observed == generation)
            assert(pthread_cond_wait(&barrier_changed, &barrier_mutex) == 0);
    }
    assert(pthread_mutex_unlock(&barrier_mutex) == 0);
}

static void *independent_worker(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    unsigned round;
    for (round = 0; round < ROUNDS; ++round) {
        unsigned char value = (unsigned char)(index + round + 1);
        unsigned char *src = cow_alloc(PAGES * page);
        unsigned char *dst = cow_alloc(PAGES * page);
        size_t i;
        assert(src && dst);
        memset(src, value, PAGES * page);
        cow_memcpy(dst, src, PAGES * page);
        for (i = 0; i < PAGES; ++i) {
            src[i * page] = 0;
            assert(dst[i * page] == value);
            dst[i * page + 1] = 0xff;
            assert(src[i * page + 1] == value);
        }
        assert(cow_free(src) == 0);
        assert(dst[PAGES * page - 1] == value);
        assert(cow_free(dst) == 0);
    }
    return NULL;
}

static void *shared_worker(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    unsigned round;
    for (round = 0; round < ROUNDS; ++round) {
        size_t p;
        barrier();
        /* All threads first fault on the same page. They then write distinct
         * bytes on each page, so there are no application-level data races.
         * Half write source-first; half write destination-first. */
        assert(shared_dst[page - 1] == 0x6d);
        for (p = 0; p < PAGES; ++p) {
            size_t offset = p * page + index;
            if (index % 2) {
                shared_src[offset] = 0x11;
                assert(shared_dst[offset] == 0x6d);
                shared_dst[offset] = 0x22;
            } else {
                shared_dst[offset] = 0x22;
                assert(shared_src[offset] == 0x6d);
                shared_src[offset] = 0x11;
            }
        }
        barrier();
    }
    return NULL;
}

static void *materializer(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&stop_materializing, memory_order_acquire))
        assert(cow_materialize() == 0);
    return NULL;
}

static void *read_worker(void *unused)
{
    unsigned round;
    (void)unused;
    for (round = 0; round < ROUNDS; ++round) {
        size_t i;
        barrier();
        for (i = 0; i < PAGES * page; i += 17)
            assert(((volatile unsigned char *)shared_dst)[i] == 0x6d);
        barrier();
    }
    return NULL;
}

static void run_shared(int concurrent_materialization)
{
    pthread_t threads[THREADS];
    pthread_t resolving_thread;
    unsigned round;
    unsigned i;
    participants = THREADS + 1;
    shared_src = cow_alloc(PAGES * page);
    shared_dst = cow_alloc(PAGES * page);
    assert(shared_src && shared_dst);
    atomic_store_explicit(&stop_materializing, 0, memory_order_release);
    if (concurrent_materialization)
        assert(pthread_create(&resolving_thread, NULL, materializer, NULL) == 0);
    for (i = 0; i < THREADS; ++i)
        assert(pthread_create(&threads[i], NULL,
                              concurrent_materialization ? read_worker : shared_worker,
                              (void *)(uintptr_t)i) == 0);
    for (round = 0; round < ROUNDS; ++round) {
        memset(shared_src, 0x6d, PAGES * page);
        memset(shared_dst, 0, PAGES * page);
        cow_memcpy(shared_dst, shared_src, PAGES * page);
        barrier();
        barrier();
    }
    for (i = 0; i < THREADS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    if (concurrent_materialization) {
        atomic_store_explicit(&stop_materializing, 1, memory_order_release);
        assert(pthread_join(resolving_thread, NULL) == 0);
    }
    assert(cow_free(shared_src) == 0);
    assert(cow_free(shared_dst) == 0);
}

int main(void)
{
    pthread_t threads[THREADS];
    unsigned i;
    if (COWCPY_SIGNAL_DIRECT && strcmp(cow_backend(), "signal") == 0) {
        puts("single-threaded signal mode: concurrent access tests skipped");
        return 0;
    }
    alarm(45);
    page = cow_page_size();
    assert(page);
    for (i = 0; i < THREADS; ++i)
        assert(pthread_create(&threads[i], NULL, independent_worker,
                              (void *)(uintptr_t)i) == 0);
    for (i = 0; i < THREADS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    run_shared(0);
    run_shared(1);
    printf("%s backend: concurrent faults, API calls, and materialization passed\n",
           cow_backend());
    return 0;
}
