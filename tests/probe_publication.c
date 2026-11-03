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

/* Deterministically expose the exact interval that makes direct filling
 * unsafe. These hooks are linked only into probe/test binaries. */
static atomic_int paused;
static atomic_int release_fill;
static atomic_int observer_faulted;
static atomic_int observer_done;
static atomic_int delay_fault;
static atomic_int fault_waiting;
static atomic_int release_fault;
static unsigned char *src;
static unsigned char *dst;
static size_t page;
static unsigned char observed;

void cow_test_before_fill(void *address)
{
    (void)address;
    atomic_store_explicit(&paused, 1, memory_order_release);
    while (!atomic_load_explicit(&release_fill, memory_order_acquire))
        ;
}

void cow_test_fault_enter(void *address)
{
    uintptr_t fault = (uintptr_t)address;
    uintptr_t base = (uintptr_t)dst;
    if (fault >= base && fault - base < page &&
        atomic_load_explicit(&delay_fault, memory_order_acquire)) {
        atomic_store_explicit(&fault_waiting, 1, memory_order_release);
        while (!atomic_load_explicit(&release_fault, memory_order_acquire))
            ;
    }
    if (atomic_load_explicit(&paused, memory_order_acquire) &&
        fault >= base && fault - base < page)
        atomic_store_explicit(&observer_faulted, 1, memory_order_release);
}

static void *writer(void *unused)
{
    (void)unused;
    src[0] = 8;
    return NULL;
}

static void *observer(void *unused)
{
    (void)unused;
    observed = ((volatile unsigned char *)dst)[page - 1];
    atomic_store_explicit(&observer_done, 1, memory_order_release);
    return NULL;
}

static void test_retired_fault(void)
{
    pthread_t thread;
    unsigned char *other_src = cow_alloc(page);
    unsigned char *other_dst = cow_alloc(page);
    src = cow_alloc(page);
    dst = cow_alloc(page);
    assert(src && dst && other_src && other_dst);
    memset(src, 0x6d, page);
    cow_memcpy(dst, src, page);
    atomic_store_explicit(&delay_fault, 1, memory_order_release);
    assert(pthread_create(&thread, NULL, observer, NULL) == 0);
    while (!atomic_load_explicit(&fault_waiting, memory_order_acquire))
        ;
    /* Retire the record while its fault handler is delayed before acquiring
     * the library lock. Replace it with a copy of completely different pages. */
    assert(cow_materialize() == 0);
    cow_memcpy(other_dst, other_src, page);
    atomic_store_explicit(&release_fault, 1, memory_order_release);
    assert(pthread_join(thread, NULL) == 0);
    assert(observed == 0x6d);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
    assert(cow_free(other_src) == 0);
    assert(cow_free(other_dst) == 0);
}

int main(void)
{
    pthread_t writing_thread;
    pthread_t reading_thread;
    int exposed;
    alarm(15);
    page = cow_page_size();
    cow_set_min_copy(0);
    if (!(cow_features() & COW_FEATURE_SIGNAL)) {
        puts("signal publication probe skipped: no usable signal backing");
        return 0;
    }
    src = cow_alloc(page);
    dst = cow_alloc(page);
    assert(src && dst);
    memset(src, 0x6d, page);
    cow_memcpy(dst, src, page);
    assert(pthread_create(&writing_thread, NULL, writer, NULL) == 0);
    while (!atomic_load_explicit(&paused, memory_order_acquire))
        ;
    assert(pthread_create(&reading_thread, NULL, observer, NULL) == 0);
    while (!atomic_load_explicit(&observer_done, memory_order_acquire) &&
           !atomic_load_explicit(&observer_faulted, memory_order_acquire))
        ;
    exposed = atomic_load_explicit(&observer_done, memory_order_acquire);
    atomic_store_explicit(&release_fill, 1, memory_order_release);
    assert(pthread_join(writing_thread, NULL) == 0);
    assert(pthread_join(reading_thread, NULL) == 0);
    assert(src[0] == 8 && dst[0] == 0x6d);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
    if (exposed) {
        assert(observed != 0x6d);
        puts("direct fill: another thread read the page before it was filled");
        return 1;
    }
    assert(observed == 0x6d);
    puts("alias fill: destination stayed protected until the copy was complete");
    test_retired_fault();
    puts("retired fault: handler safely retried after its record was replaced");
    return 0;
}
