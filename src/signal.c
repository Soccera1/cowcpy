#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define cow_alloc cow_signal_alloc
#define cow_free cow_signal_free
#define cow_memcpy cow_signal_memcpy
#define cow_materialize cow_signal_materialize
#define cow_page_size cow_signal_page_size
#define cow_backend cow_signal_backend
#define COWCPY_FORCE_SIGNAL 1
#include "cowcpy.h"
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef COWCPY_HAVE_CONFIG
#include "cowcpy_config.h"
#endif
#ifndef COWCPY_SIGNAL_DIRECT
#define COWCPY_SIGNAL_DIRECT 0
#endif
#ifndef COWCPY_SIGNAL_LAZY
#define COWCPY_SIGNAL_LAZY 1
#endif

struct allocation {
    unsigned char *base;
    size_t size;
    size_t mapped;
    unsigned flags;
#if !COWCPY_SIGNAL_DIRECT
    unsigned char *alias;
    int backing_fd;
#endif
    struct allocation *next;
};

static struct allocation *allocations;
static size_t page_size;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

/* This opt-in backend requires host support for mutex operations inside a
 * controlled synchronous handler. It is never selected by POSIX auto mode.
 * API code resolves protected pages before accessing caller memory. */
static void lock_state(void)
{
    if (pthread_mutex_lock(&state_lock) != 0)
        _exit(127);
}

static void unlock_state(void)
{
    if (pthread_mutex_unlock(&state_lock) != 0)
        _exit(127);
}

static size_t get_page_size(void)
{
    if (!page_size) {
        long value = sysconf(_SC_PAGESIZE);
        if (value <= 0) {
            errno = ENOSYS;
            return 0;
        }
        page_size = (size_t)value;
    }
    return page_size;
}

const char *cow_backend(void)
{
    return COWCPY_SIGNAL_LAZY ? "signal" : "eager";
}

static struct allocation *find_allocation(const void *ptr, size_t size)
{
    uintptr_t address = (uintptr_t)ptr;
    struct allocation *item;
    for (item = allocations; item; item = item->next) {
        uintptr_t base = (uintptr_t)item->base;
        if (address >= base && address - base <= item->size &&
            size <= item->size - (size_t)(address - base))
            return item;
    }
    return NULL;
}

struct deferred_page {
    unsigned char *src;
    unsigned char *dst;
    unsigned char *fill;
    int active;
};

struct deferred_batch {
    struct deferred_page *pages;
    size_t count;
    struct deferred_batch *next;
};
static struct deferred_batch *batches;
static struct sigaction previous_segv;
static int handler_installed;

#ifdef COWCPY_TEST_HOOKS
/* Test-only hooks deliberately hold the fill window open. */
extern void cow_test_before_fill(void *dst);
extern void cow_test_fault_enter(void *address);
#endif

/* This explicitly enabled backend requires host support beyond POSIX for
 * mprotect and the mutex in a synchronous protection-fault handler. */
static int resolve_page(struct deferred_page *page)
{
    volatile unsigned char *dst;
    const volatile unsigned char *src;
    size_t i;
    if (!page->active)
        return 0;
#if COWCPY_SIGNAL_DIRECT
    if (mprotect(page->dst, page_size, PROT_READ | PROT_WRITE) < 0)
        return -1;
#endif
#ifdef COWCPY_TEST_HOOKS
    cow_test_before_fill(page->dst);
#endif
    dst = page->fill;
    src = page->src;
    for (i = 0; i < page_size; ++i)
        dst[i] = src[i];
#if !COWCPY_SIGNAL_DIRECT
    /* Keep the public address inaccessible until all bytes are initialized.
     * Other threads either see the completed page or fault and wait. */
    if (mprotect(page->dst, page_size, PROT_READ | PROT_WRITE) < 0)
        return -1;
#endif
    if (mprotect(page->src, page_size, PROT_READ | PROT_WRITE) < 0)
        return -1;
    page->active = 0;
    return 0;
}

static void segv_handler(int signal_number, siginfo_t *info, void *context)
{
    uintptr_t fault = (uintptr_t)info->si_addr;
    int saved_errno = errno;
    size_t i;
    struct deferred_batch *batch;
    (void)context;
#ifdef COWCPY_TEST_HOOKS
    cow_test_fault_enter(info->si_addr);
#endif
    if (info->si_code == SEGV_ACCERR) {
        lock_state();
        for (batch = batches; batch; batch = batch->next) {
          for (i = 0; i < batch->count; ++i) {
            struct deferred_page *page = &batch->pages[i];
            uintptr_t src = (uintptr_t)page->src;
            uintptr_t dst = (uintptr_t)page->dst;
            if ((fault >= src && fault - src < page_size) ||
                (fault >= dst && fault - dst < page_size)) {
                if (resolve_page(page) < 0)
                    _exit(128 + signal_number);
                unlock_state();
                errno = saved_errno;
                return;
            }
          }
        }
        /* The fault may have been delivered before another handler or API
         * call resolved and retired its record. A live owned address is now
         * accessible; retry it without touching the retired metadata. */
        if (find_allocation(info->si_addr, 1)) {
            unlock_state();
            errno = saved_errno;
            return;
        }
        unlock_state();
    }
    /* A pre-existing custom handler is never displaced (see install_handler).
     * Re-raise with the default disposition, including for user-sent signals. */
    if (sigaction(signal_number, &previous_segv, NULL) < 0)
        _exit(128 + signal_number);
    if (raise(signal_number) != 0)
        _exit(128 + signal_number);
    errno = saved_errno;
}

static int install_handler(void)
{
    struct sigaction action;
    if (handler_installed) {
        if (sigaction(SIGSEGV, NULL, &action) < 0)
            return -1;
        if (!(action.sa_flags & SA_SIGINFO) ||
            action.sa_sigaction != segv_handler) {
            errno = EBUSY;
            return -1;
        }
        return 0;
    }
    if (sigaction(SIGSEGV, NULL, &previous_segv) < 0)
        return -1;
    if (previous_segv.sa_handler != SIG_DFL) {
        errno = EBUSY;
        return -1;
    }
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = segv_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) < 0)
        return -1;
    handler_installed = 1;
    return 0;
}

static int materialize_locked(void)
{
    size_t i;
    while (batches) {
        struct deferred_batch *batch = batches;
        for (i = 0; i < batch->count; ++i) {
            if (resolve_page(&batch->pages[i]) < 0)
                return -1;
        }
        batches = batch->next;
        free(batch->pages);
        free(batch);
    }
    return 0;
}

static int intersects(const void *ptr, size_t size, const void *page)
{
    uintptr_t first = (uintptr_t)ptr;
    uintptr_t second = (uintptr_t)page;
    if (!size)
        return 0;
    return first >= second ? first - second < page_size : second - first < size;
}

static int materialize_range_locked(const void *ptr, size_t size)
{
    struct deferred_batch **link = &batches;
    while (*link) {
        struct deferred_batch *batch = *link;
        size_t i;
        int active = 0;
        for (i = 0; i < batch->count; ++i) {
            struct deferred_page *page = &batch->pages[i];
            if (page->active && (intersects(ptr, size, page->src) ||
                                 intersects(ptr, size, page->dst))) {
                if (resolve_page(page) < 0)
                    return -1;
            }
            active |= page->active;
        }
        if (!active) {
            *link = batch->next;
            free(batch->pages);
            free(batch);
        } else {
            link = &batch->next;
        }
    }
    return 0;
}

static int lazy_copy(struct allocation *src_alloc,
                     struct allocation *dst_alloc,
                     unsigned char *dst, unsigned char *src, size_t size)
{
    size_t i;
    struct deferred_batch *batch;
    (void)src_alloc;
#if COWCPY_SIGNAL_DIRECT
    (void)dst_alloc;
#endif
    if (!COWCPY_SIGNAL_LAZY)
        return -1;
    if (src_alloc->flags || dst_alloc->flags)
        return -1;
#if !COWCPY_SIGNAL_DIRECT
    if (!dst_alloc->alias)
        return -1;
#endif
    if (install_handler() < 0)
        return -1;
    batch = calloc(1, sizeof(*batch));
    if (!batch)
        return -1;
    batch->pages = calloc(size / page_size, sizeof(*batch->pages));
    if (!batch->pages) {
        free(batch);
        return -1;
    }
    batch->next = batches;
    batches = batch;
    for (i = 0; i < size / page_size; ++i) {
        struct deferred_page *page = &batch->pages[i];
        page->src = src + i * page_size;
        page->dst = dst + i * page_size;
#if COWCPY_SIGNAL_DIRECT
        page->fill = page->dst;
#else
        page->fill = dst_alloc->alias + (page->dst - dst_alloc->base);
#endif
        if (mprotect(page->src, page_size, PROT_READ) < 0)
            break;
        if (mprotect(page->dst, page_size, PROT_NONE) < 0) {
            /* Failure to restore a permission cannot be hidden by memcpy. */
            if (mprotect(page->src, page_size, PROT_READ | PROT_WRITE) < 0)
                abort();
            break;
        }
        page->active = 1;
        batch->count = i + 1;
    }
    if (i != size / page_size) {
        size_t j;
        for (j = 0; j < batch->count; ++j) {
            if (resolve_page(&batch->pages[j]) < 0)
                abort();
        }
        batches = batch->next;
        free(batch->pages);
        free(batch);
        return -1;
    }
    return 0;
}

static void *anonymous_mapping(size_t size)
{
#ifdef MAP_ANONYMOUS
    return mmap(NULL, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    int fd = open("/dev/zero", O_RDWR);
    void *result;
    int saved;
    if (fd < 0)
        return MAP_FAILED;
    result = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    saved = errno;
    close(fd);
    errno = saved;
    return result;
#endif
}

static void *alloc_locked(size_t size, unsigned flags)
{
    struct allocation *item;
    size_t page = get_page_size();
    size_t mapped;
    int saved_errno;
    if (!page)
        return NULL;
    if (!size || flags & ~COW_ALLOC_EAGER || size > SIZE_MAX - (page - 1)) {
        errno = !size || (flags & ~COW_ALLOC_EAGER) ? EINVAL : ENOMEM;
        return NULL;
    }
    mapped = ((size + page - 1) / page) * page;
    item = calloc(1, sizeof(*item));
    if (!item)
        return NULL;
    item->flags = flags;
#if !COWCPY_SIGNAL_DIRECT
    item->backing_fd = -1;
#endif
    if (flags & COW_ALLOC_EAGER) {
        item->base = anonymous_mapping(mapped);
        goto mapping_ready;
    }
#if COWCPY_SIGNAL_DIRECT
    item->base = anonymous_mapping(mapped);
#else
    {
        int fd;
        item->backing_fd = -1;
        fd = cow_backing_create(mapped);
        if (fd < 0) {
            item->base = anonymous_mapping(mapped);
            goto mapping_ready;
        }
        item->alias = mmap(NULL, mapped, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
        if (item->alias == MAP_FAILED) {
            item->alias = NULL;
            item->base = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        } else {
            item->base = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        item->backing_fd = fd;
    }
#endif
mapping_ready:
    if (item->base == MAP_FAILED) {
        saved_errno = errno;
#if !COWCPY_SIGNAL_DIRECT
        if (item->alias)
            munmap(item->alias, mapped);
        if (item->backing_fd >= 0)
            close(item->backing_fd);
#endif
        free(item);
        errno = saved_errno;
        return NULL;
    }
    item->size = size;
    item->mapped = mapped;
    item->next = allocations;
    allocations = item;
    return item->base;
}

static int free_locked(void *ptr)
{
    struct allocation **link;
    struct allocation *item;
    if (!ptr)
        return 0;
    for (link = &allocations; *link; link = &(*link)->next) {
        if ((*link)->base == ptr)
            break;
    }
    if (!*link) {
        errno = EINVAL;
        return -1;
    }
    if (materialize_range_locked((*link)->base, (*link)->mapped) < 0)
        return -1;
    item = *link;
    if (munmap(item->base, item->mapped) < 0)
        return -1;
    *link = item->next;
#if !COWCPY_SIGNAL_DIRECT
    if (item->alias && munmap(item->alias, item->mapped) < 0)
        abort();
    if (item->backing_fd >= 0)
        close(item->backing_fd);
#endif
    free(item);
    return 0;
}

static void *copy_locked(void *restrict dst, const void *restrict src, size_t size)
{
    struct allocation *src_alloc;
    struct allocation *dst_alloc;
    size_t page;
    size_t head;
    size_t body;
    int saved_errno = errno;
    if (!size)
        return dst;
    /* Resolve only dependencies; unrelated copies remain deferred. */
    if (materialize_range_locked(src, size) < 0 ||
        materialize_range_locked(dst, size) < 0)
        abort();
    page = get_page_size();
    src_alloc = find_allocation(src, size);
    dst_alloc = find_allocation(dst, size);
    if (!page || !src_alloc || !dst_alloc ||
        (uintptr_t)src % page != (uintptr_t)dst % page)
        goto eager;
    head = (page - (uintptr_t)dst % page) % page;
    if (head > size)
        head = size;
    body = ((size - head) / page) * page;
    if (!body)
        goto eager;
    if (head)
        memcpy(dst, src, head);
    if (lazy_copy(src_alloc, dst_alloc, (unsigned char *)dst + head,
                  (unsigned char *)src + head, body) < 0)
        memcpy((unsigned char *)dst + head,
               (const unsigned char *)src + head, body);
    if (size > head + body)
        memcpy((unsigned char *)dst + head + body,
               (const unsigned char *)src + head + body, size - head - body);
    errno = saved_errno;
    return dst;

eager:
    memcpy(dst, src, size);
    errno = saved_errno;
    return dst;
}

size_t cow_page_size(void)
{
    size_t result;
    lock_state();
    result = get_page_size();
    unlock_state();
    return result;
}

void *cow_alloc(size_t size)
{
    void *result;
    lock_state();
    result = alloc_locked(size, 0);
    unlock_state();
    return result;
}

void *cow_signal_alloc_ex(size_t size, unsigned flags)
{
    void *result;
    lock_state();
    result = alloc_locked(size, flags);
    unlock_state();
    return result;
}

int cow_free(void *ptr)
{
    int result;
    lock_state();
    result = free_locked(ptr);
    unlock_state();
    return result;
}

void *cow_memcpy(void *restrict dst, const void *restrict src, size_t size)
{
    void *result;
    lock_state();
    result = copy_locked(dst, src, size);
    unlock_state();
    return result;
}

int cow_materialize(void)
{
    int result;
    lock_state();
    result = materialize_locked();
    unlock_state();
    return result;
}

int cow_signal_materialize_range(const void *ptr, size_t size)
{
    int result;
    lock_state();
    result = materialize_range_locked(ptr, size);
    unlock_state();
    return result;
}
