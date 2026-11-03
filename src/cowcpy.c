#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cowcpy.h"
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef COWCPY_HAVE_CONFIG
#include "cowcpy_config.h"
#endif
#ifndef COW_HAVE_MEMFD
#ifdef __linux__
#define COW_HAVE_MEMFD 1
#else
#define COW_HAVE_MEMFD 0
#endif
#endif
#ifndef COW_HAVE_MACH
#ifdef __APPLE__
#define COW_HAVE_MACH 1
#else
#define COW_HAVE_MACH 0
#endif
#endif
#ifndef COW_HAVE_SHM
#define COW_HAVE_SHM 1
#endif
#ifndef COW_BACKEND_REQUEST
#define COW_BACKEND_REQUEST 0
#endif
#ifndef COW_BACKING_REQUEST
#define COW_BACKING_REQUEST 0
#endif
#ifndef COW_ALLOW_DISK
#define COW_ALLOW_DISK 0
#endif
#ifndef COW_FIXED_REMAP
#define COW_FIXED_REMAP 1
#endif
#ifndef COW_MIN_PAGES
#define COW_MIN_PAGES 4
#endif
#ifndef COW_MUTABLE_SNAPSHOTS
#define COW_MUTABLE_SNAPSHOTS 1
#endif
#if COW_HAVE_MACH
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#ifdef COWCPY_TEST_VM
extern void *cow_test_mmap(void *, size_t, int, int, int, off_t);
#define mmap cow_test_mmap
#endif

enum backend { EAGER, MAPPED, MACH, SIGNAL };
struct allocation {
    unsigned char *base;
    size_t size;
    size_t mapped;
    unsigned flags;
    unsigned refs;
    int frozen;
    int snapshot;
    int poisoned;
    pthread_mutex_t mutex;
    struct allocation *next;
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t registry_changed = PTHREAD_COND_INITIALIZER;
static struct allocation *allocations;
static unsigned active_calls;
static int fork_pending;
static size_t page_size;
static int initialization_error;
static enum backend selected;
static const char *selected_name = "eager";
static atomic_size_t minimum_copy;
#if COW_HAVE_SHM
static atomic_ulong backing_serial;
#endif

static int resize_backing(int fd, size_t size)
{
    off_t length = (off_t)size;
    if (length < 0 || (uintmax_t)length != (uintmax_t)size) {
        errno = EOVERFLOW;
        return -1;
    }
    if (ftruncate(fd, length) < 0)
        return -1;
    return fd;
}

static int finish_backing(int fd, size_t size)
{
    int saved;
    if (fd < 0)
        return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0 && resize_backing(fd, size) >= 0)
        return fd;
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

static int shm_backing(size_t size)
{
#if COW_HAVE_SHM
    unsigned attempt;
    for (attempt = 0; attempt < 32; ++attempt) {
        char name[96];
        unsigned long serial = atomic_fetch_add(&backing_serial, 1);
        int fd;
        snprintf(name, sizeof(name), "/cowcpy-%lu-%lu",
                 (unsigned long)getpid(), serial);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return -1;
        }
        if (shm_unlink(name) < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        return finish_backing(fd, size);
    }
    errno = EEXIST;
#else
    (void)size;
    errno = ENOSYS;
#endif
    return -1;
}

static int disk_backing(size_t size)
{
    const char *directory = getenv("TMPDIR");
    char *name;
    size_t length;
    int fd;
    int saved;
    if (!directory || !*directory)
        directory = "/tmp";
    length = strlen(directory);
    if (length > SIZE_MAX - 24) {
        errno = ENAMETOOLONG;
        return -1;
    }
    name = malloc(length + 24);
    if (!name)
        return -1;
    snprintf(name, length + 24, "%s/cowcpy-XXXXXX", directory);
    fd = mkstemp(name);
    saved = errno;
    if (fd >= 0 && unlink(name) < 0) {
        saved = errno;
        close(fd);
        fd = -1;
    }
    free(name);
    errno = saved;
    return finish_backing(fd, size);
}

/* Backings are immutable once published in private mappings. Neither the
 * portable backend nor the Linux backend needs a shared public allocation. */
int cow_backing_create(size_t size)
{
    int fd;
    if (COW_BACKING_REQUEST == 4) {
        errno = ENOTSUP;
        return -1;
    }
#if COW_HAVE_MEMFD
    if (COW_BACKING_REQUEST == 0 || COW_BACKING_REQUEST == 1) {
        fd = finish_backing(memfd_create("cowcpy", MFD_CLOEXEC), size);
        if (fd >= 0)
            return fd;
        if (COW_BACKING_REQUEST == 1)
            return -1;
    }
#endif
    if (COW_BACKING_REQUEST == 0 || COW_BACKING_REQUEST == 2) {
        fd = shm_backing(size);
        if (fd >= 0)
            return fd;
        if (COW_BACKING_REQUEST == 2)
            return -1;
    }
    if (COW_BACKING_REQUEST == 3 || COW_ALLOW_DISK)
        return disk_backing(size);
    errno = ENOTSUP;
    return -1;
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

static void before_fork(void)
{
    pthread_mutex_lock(&registry_mutex);
    fork_pending = 1;
    while (active_calls)
        pthread_cond_wait(&registry_changed, &registry_mutex);
}

static void after_fork(void)
{
    fork_pending = 0;
    pthread_cond_broadcast(&registry_changed);
    pthread_mutex_unlock(&registry_mutex);
}

#if COW_HAVE_MACH
static int mach_usable(void)
{
    unsigned char *a = anonymous_mapping(page_size);
    unsigned char *b = anonymous_mapping(page_size);
    int result = 0;
    if (a != MAP_FAILED && b != MAP_FAILED) {
        a[0] = 19;
        if (mach_vm_copy(mach_task_self(), (mach_vm_address_t)(uintptr_t)a,
                         (mach_vm_size_t)page_size,
                         (mach_vm_address_t)(uintptr_t)b) == KERN_SUCCESS) {
            a[0] = 23;
            result = b[0] == 19;
        }
    }
    if (a != MAP_FAILED)
        munmap(a, page_size);
    if (b != MAP_FAILED)
        munmap(b, page_size);
    return result;
}
#endif

static void initialize(void)
{
    long value = sysconf(_SC_PAGESIZE);
    int fd;
    int result;
    if (value <= 0) {
        initialization_error = ENOSYS;
        return;
    }
    page_size = (size_t)value;
#if COW_MIN_PAGES == 0
    atomic_store(&minimum_copy, 0);
#else
    atomic_store(&minimum_copy, COW_MIN_PAGES > SIZE_MAX / page_size ?
                 SIZE_MAX : COW_MIN_PAGES * page_size);
#endif
#if defined(COWCPY_FORCE_SIGNAL)
    selected = SIGNAL;
#else
    if (COW_BACKEND_REQUEST == 3)
        selected = SIGNAL;
    else if (COW_BACKEND_REQUEST != 4) {
#if COW_HAVE_MACH
        if ((COW_BACKEND_REQUEST == 0 || COW_BACKEND_REQUEST == 2) && mach_usable()) {
            selected = MACH;
            selected_name = "mach";
        } else
#endif
        {
            fd = cow_backing_create(page_size);
            if (fd >= 0) {
                close(fd);
                selected = MAPPED;
                selected_name = "mapped";
            }
        }
    }
#endif
    (void)fd;
    if (selected == SIGNAL) {
        selected_name = cow_signal_backend();
        if (strcmp(selected_name, "signal") == 0) {
            fd = cow_backing_create(page_size);
            if (fd >= 0) {
                close(fd);
                return;
            }
        }
        selected = EAGER;
        selected_name = "eager";
    }
    result = pthread_atfork(before_fork, after_fork, after_fork);
    if (result)
        initialization_error = result;
}

static int ensure_initialized(void)
{
    int result = pthread_once(&once, initialize);
    if (result || initialization_error) {
        errno = result ? result : initialization_error;
        return -1;
    }
    return 0;
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

static void enter_call(void)
{
    pthread_mutex_lock(&registry_mutex);
    while (fork_pending)
        pthread_cond_wait(&registry_changed, &registry_mutex);
    ++active_calls;
    pthread_mutex_unlock(&registry_mutex);
}

static void leave_call(void)
{
    pthread_mutex_lock(&registry_mutex);
    --active_calls;
    pthread_cond_broadcast(&registry_changed);
    pthread_mutex_unlock(&registry_mutex);
}

static void lock_pair(struct allocation *a, struct allocation *b)
{
    if (a && b && (uintptr_t)a->base > (uintptr_t)b->base) {
        struct allocation *temporary = a;
        a = b;
        b = temporary;
    }
    if (a)
        pthread_mutex_lock(&a->mutex);
    if (b && b != a)
        pthread_mutex_lock(&b->mutex);
}

static void unlock_pair(struct allocation *a, struct allocation *b)
{
    if (a)
        pthread_mutex_unlock(&a->mutex);
    if (b && b != a)
        pthread_mutex_unlock(&b->mutex);
    pthread_mutex_lock(&registry_mutex);
    if (a)
        --a->refs;
    if (b && b != a)
        --b->refs;
    --active_calls;
    pthread_cond_broadcast(&registry_changed);
    pthread_mutex_unlock(&registry_mutex);
}

static void acquire_pair(const void *src, size_t src_size, const void *dst,
                         size_t dst_size, struct allocation **a,
                         struct allocation **b)
{
    pthread_mutex_lock(&registry_mutex);
    while (fork_pending)
        pthread_cond_wait(&registry_changed, &registry_mutex);
    ++active_calls;
    *a = find_allocation(src, src_size);
    *b = find_allocation(dst, dst_size);
    if (*a)
        ++(*a)->refs;
    if (*b && *b != *a)
        ++(*b)->refs;
    pthread_mutex_unlock(&registry_mutex);
    lock_pair(*a, *b);
}

static int write_snapshot(int fd, const void *src, size_t size)
{
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        ssize_t result;
        if (chunk > (size_t)SSIZE_MAX)
            chunk = (size_t)SSIZE_MAX;
        result = pwrite(fd, (const unsigned char *)src + done, chunk, (off_t)done);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            if (!result)
                errno = EIO;
            return -1;
        }
        done += (size_t)result;
    }
    return 0;
}

static int make_snapshot(const void *src, size_t size)
{
    int fd = cow_backing_create(size);
    if (fd >= 0 && write_snapshot(fd, src, size) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/* Fixed replacement cannot be made infallible on POSIX. Keep an independent
 * preflight view and attempt to restore the intended contents on failure.
 * Checked copies and cow_memdup never take this destructive path. */
static int replace_mapping(void *dst, size_t size, int fd, off_t offset,
                           struct allocation *owner, int protection)
{
    void *preflight = mmap(NULL, size, protection, MAP_PRIVATE, fd, offset);
    void *result;
    int saved;
    if (preflight == MAP_FAILED)
        return -1;
    result = mmap(dst, size, protection, MAP_PRIVATE | MAP_FIXED, fd, offset);
    saved = errno;
    munmap(preflight, size);
    if (result != MAP_FAILED)
        return 0;
    /* Releasing the preflight VMA may make recovery possible under a VMA
     * limit. Mapping the intended snapshot completes the requested copy. */
    result = mmap(dst, size, protection, MAP_PRIVATE | MAP_FIXED, fd, offset);
    if (result != MAP_FAILED)
        return 0;
    owner->poisoned = 1;
    errno = saved;
    return -2;
}

size_t cow_page_size(void)
{
    return ensure_initialized() < 0 ? 0 : page_size;
}

const char *cow_backend(void)
{
    if (ensure_initialized() < 0)
        return "eager";
    return selected_name;
}

void cow_set_min_copy(size_t size)
{
    if (ensure_initialized() == 0)
        atomic_store(&minimum_copy, size);
}

unsigned long cow_features(void)
{
    unsigned long result = COW_FEATURE_CHECKED | COW_FEATURE_MEMDUP;
    if (ensure_initialized() < 0)
        return 0;
    if (selected == SIGNAL)
        return result | COW_FEATURE_SIGNAL;
    result |= COW_FEATURE_FORK | COW_FEATURE_FREEZE;
    if (selected == MACH)
        result |= COW_FEATURE_NATIVE;
    if (selected == MAPPED && COW_FIXED_REMAP)
        result |= COW_FEATURE_REMAP;
    return result;
}

void *cow_alloc_ex(size_t size, unsigned flags)
{
    struct allocation *item;
    int error;
    if (ensure_initialized() < 0)
        return NULL;
    if (selected == SIGNAL) {
        return cow_signal_alloc_ex(size, flags);
    }
    if (flags & ~COW_ALLOC_EAGER || !size || size > SIZE_MAX - (page_size - 1)) {
        errno = !size || (flags & ~COW_ALLOC_EAGER) ? EINVAL : ENOMEM;
        return NULL;
    }
    enter_call();
    item = calloc(1, sizeof(*item));
    if (!item) {
        leave_call();
        return NULL;
    }
    item->mapped = ((size + page_size - 1) / page_size) * page_size;
    item->snapshot = -1;
    item->size = size;
    item->flags = flags;
    error = pthread_mutex_init(&item->mutex, NULL);
    if (error) {
        free(item);
        leave_call();
        errno = error;
        return NULL;
    }
    item->base = anonymous_mapping(item->mapped);
    if (item->base == MAP_FAILED) {
        int saved = errno;
        pthread_mutex_destroy(&item->mutex);
        free(item);
        leave_call();
        errno = saved;
        return NULL;
    }
    pthread_mutex_lock(&registry_mutex);
    item->next = allocations;
    allocations = item;
    pthread_mutex_unlock(&registry_mutex);
    leave_call();
    return item->base;
}

void *cow_alloc(size_t size)
{
    return cow_alloc_ex(size, 0);
}

int cow_free(void *ptr)
{
    struct allocation **link;
    struct allocation *item;
    int saved;
    if (!ptr)
        return 0;
    if (ensure_initialized() < 0)
        return -1;
    if (selected == SIGNAL)
        return cow_signal_free(ptr);
    pthread_mutex_lock(&registry_mutex);
    while (fork_pending)
        pthread_cond_wait(&registry_changed, &registry_mutex);
    for (link = &allocations; *link && (*link)->base != ptr; link = &(*link)->next)
        ;
    if (!*link) {
        pthread_mutex_unlock(&registry_mutex);
        errno = EINVAL;
        return -1;
    }
    item = *link;
    *link = item->next;
    ++active_calls;
    while (item->refs)
        pthread_cond_wait(&registry_changed, &registry_mutex);
    pthread_mutex_unlock(&registry_mutex);
    if (munmap(item->base, item->mapped) < 0) {
        saved = errno;
        pthread_mutex_lock(&registry_mutex);
        item->next = allocations;
        allocations = item;
        pthread_mutex_unlock(&registry_mutex);
        leave_call();
        errno = saved;
        return -1;
    }
    if (item->snapshot >= 0)
        close(item->snapshot);
    pthread_mutex_destroy(&item->mutex);
    free(item);
    leave_call();
    return 0;
}

static int copy_native(void *dst, const void *src, size_t size, int checked)
{
    struct allocation *a;
    struct allocation *b;
    size_t head;
    size_t body;
    int fd = -1;
    int owned_fd = 0;
    int result = 0;
    off_t offset = 0;
    acquire_pair(src, size, dst, size, &a, &b);
    if ((a && a->poisoned) || (b && b->poisoned)) {
        errno = EFAULT;
        result = -1;
        goto done;
    }
    if (b && b->frozen) {
        errno = EPERM;
        result = -1;
        goto done;
    }
    if (selected == EAGER || size < atomic_load(&minimum_copy) ||
        !b || (b->flags & COW_ALLOC_EAGER) || (a && (a->flags & COW_ALLOC_EAGER)))
        goto eager;
    head = (page_size - (uintptr_t)dst % page_size) % page_size;
    if (head > size)
        head = size;
    body = ((size - head) / page_size) * page_size;
    if (!body)
        goto eager;
#if COW_HAVE_MACH
    if (selected == MACH && (uintptr_t)src % page_size == (uintptr_t)dst % page_size) {
        kern_return_t status = mach_vm_copy(mach_task_self(),
            (mach_vm_address_t)((uintptr_t)src + head), (mach_vm_size_t)body,
            (mach_vm_address_t)((uintptr_t)dst + head));
        if (status == KERN_SUCCESS) {
            if (head)
                memcpy(dst, src, head);
            if (size > head + body)
                memcpy((unsigned char *)dst + head + body,
                       (const unsigned char *)src + head + body, size - head - body);
            goto done;
        }
        goto eager;
    }
#endif
    if (checked || !COW_FIXED_REMAP || selected != MAPPED || !a ||
        (a->flags & COW_ALLOC_EAGER) ||
        (!a->frozen && !COW_MUTABLE_SNAPSHOTS) ||
        (uintptr_t)src % page_size != (uintptr_t)dst % page_size)
        goto eager;
    if (a->frozen && a->snapshot >= 0) {
        fd = a->snapshot;
        offset = (off_t)((const unsigned char *)src + head - a->base);
    } else {
        fd = make_snapshot((const unsigned char *)src + head, body);
        if (fd < 0)
            goto eager;
        owned_fd = 1;
        /* Both sides share this immutable snapshot. Ordinary writes are
         * private; future mutable snapshots must capture current bytes. */
        result = replace_mapping((unsigned char *)a->base +
            ((const unsigned char *)src + head - a->base), body, fd, 0, a,
            a->frozen ? PROT_READ : PROT_READ | PROT_WRITE);
        if (result == -2)
            goto done;
        if (result < 0)
            goto eager;
    }
    result = replace_mapping((unsigned char *)dst + head, body, fd, offset, b,
                             PROT_READ | PROT_WRITE);
    if (result == -2)
        goto done;
    if (result < 0)
        goto eager;
    if (head)
        memcpy(dst, src, head);
    if (size > head + body)
        memcpy((unsigned char *)dst + head + body,
               (const unsigned char *)src + head + body, size - head - body);
    result = 0;
    goto done;

eager:
    memcpy(dst, src, size);
    result = 0;
done:
    if (owned_fd)
        close(fd);
    unlock_pair(a, b);
    return result;
}

int cow_memcpy_checked(void *dst, const void *src, size_t size)
{
    int saved = errno;
    int result;
    if (!size)
        return 0;
    if (ensure_initialized() < 0)
        return -1;
    if (selected == SIGNAL) {
        if (cow_signal_materialize_range(src, size) < 0 ||
            cow_signal_materialize_range(dst, size) < 0)
            return -1;
        memcpy(dst, src, size);
        result = 0;
    } else {
        result = copy_native(dst, src, size, 1);
    }
    if (!result)
        errno = saved;
    return result < 0 ? -1 : 0;
}

void *cow_memcpy(void *restrict dst, const void *restrict src, size_t size)
{
    int saved = errno;
    if (!size)
        return dst;
    if (ensure_initialized() < 0) {
        memcpy(dst, src, size);
    } else if (selected == SIGNAL) {
        if (size < atomic_load(&minimum_copy)) {
            if (cow_memcpy_checked(dst, src, size) < 0)
                abort();
        } else {
            cow_signal_memcpy(dst, src, size);
        }
    } else if (size < atomic_load(&minimum_copy)) {
        memcpy(dst, src, size);
    } else if (copy_native(dst, src, size, 0) < 0) {
        /* Only an invalid frozen destination or unrecoverable VM replacement
         * can reach this point. Use checked copies to avoid fixed remaps. */
        abort();
    }
    errno = saved;
    return dst;
}

int cow_materialize_range(const void *ptr, size_t size)
{
    if (ensure_initialized() < 0)
        return -1;
    return selected == SIGNAL ? cow_signal_materialize_range(ptr, size) : 0;
}

int cow_materialize(void)
{
    if (ensure_initialized() < 0)
        return -1;
    return selected == SIGNAL ? cow_signal_materialize() : 0;
}

int cow_freeze(void *ptr)
{
    struct allocation *a;
    struct allocation *unused;
    int result = -1;
    int fd;
    if (ensure_initialized() < 0)
        return -1;
    if (selected == SIGNAL) {
        errno = ENOTSUP;
        return -1;
    }
    acquire_pair(ptr, 1, NULL, 0, &a, &unused);
    if (!a || a->base != ptr) {
        errno = EINVAL;
        goto done;
    }
    if (a->frozen) {
        result = 0;
        goto done;
    }
    fd = make_snapshot(a->base, a->mapped);
    if (fd < 0)
        goto done;
    if (mprotect(a->base, a->mapped, PROT_READ) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        goto done;
    }
    a->snapshot = fd;
    a->frozen = 1;
    result = 0;
done:
    unlock_pair(a, unused);
    return result;
}

int cow_thaw(void *ptr)
{
    struct allocation *a;
    struct allocation *unused;
    int result = -1;
    if (ensure_initialized() < 0)
        return -1;
    if (selected == SIGNAL) {
        errno = ENOTSUP;
        return -1;
    }
    acquire_pair(ptr, 1, NULL, 0, &a, &unused);
    if (!a || a->base != ptr) {
        errno = EINVAL;
    } else if (!a->frozen) {
        result = 0;
    } else if (mprotect(a->base, a->mapped, PROT_READ | PROT_WRITE) == 0) {
        close(a->snapshot);
        a->snapshot = -1;
        a->frozen = 0;
        result = 0;
    }
    unlock_pair(a, unused);
    return result;
}

void *cow_memdup(const void *src, size_t size)
{
    struct allocation *a;
    struct allocation *unused;
    struct allocation *copy;
    void *mapping;
    void *result;
    int fd;
    int owned = 0;
    int saved;
    off_t offset = 0;
    size_t mapped;
    if (ensure_initialized() < 0)
        return NULL;
    if (!size || size > SIZE_MAX - (page_size - 1)) {
        errno = size ? ENOMEM : EINVAL;
        return NULL;
    }
    if (selected == SIGNAL) {
        result = cow_alloc(size);
        if (result)
            cow_memcpy(result, src, size);
        return result;
    }
    if (selected == EAGER || size < atomic_load(&minimum_copy)) {
        result = cow_alloc(size);
        if (result)
            memcpy(result, src, size);
        return result;
    }
    mapped = ((size + page_size - 1) / page_size) * page_size;
    acquire_pair(src, size, NULL, 0, &a, &unused);
    if (a && a->frozen && a->snapshot >= 0 && (uintptr_t)src % page_size == 0) {
        fd = a->snapshot;
        offset = (off_t)((const unsigned char *)src - a->base);
    } else {
        fd = make_snapshot(src, size);
        owned = 1;
    }
    if (fd < 0) {
        unlock_pair(a, unused);
        result = cow_alloc(size);
        if (result)
            memcpy(result, src, size);
        return result;
    }
    mapping = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, offset);
    saved = errno;
    if (owned)
        close(fd);
    if (mapping == MAP_FAILED) {
        unlock_pair(a, unused);
        errno = saved;
        return NULL;
    }
    copy = calloc(1, sizeof(*copy));
    if (!copy) {
        saved = errno;
        munmap(mapping, mapped);
        unlock_pair(a, unused);
        errno = saved;
        return NULL;
    }
    saved = pthread_mutex_init(&copy->mutex, NULL);
    if (saved) {
        munmap(mapping, mapped);
        free(copy);
        unlock_pair(a, unused);
        errno = saved;
        return NULL;
    }
    copy->base = mapping;
    copy->size = size;
    copy->mapped = mapped;
    copy->snapshot = -1;
    pthread_mutex_lock(&registry_mutex);
    copy->next = allocations;
    allocations = copy;
    pthread_mutex_unlock(&registry_mutex);
    unlock_pair(a, unused);
    return mapping;
}
