#define _GNU_SOURCE
#include "cowcpy.h"
#include "internal.h"
#ifdef COWCPY_HAVE_CONFIG
#include "cowcpy_config.h"
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int interrupted;
static unsigned atomic_creations;
static char retained_name[1024];

static int maybe_fail(const char *name)
{
    assert(strlen(name) < sizeof(retained_name));
    strcpy(retained_name, name);
    if (interrupted) {
        --interrupted;
        errno = EINTR;
        return 1;
    }
    if (failures) {
        --failures;
        errno = EACCES;
        return 1;
    }
    return 0;
}

int cow_test_unlink(const char *name)
{
    return maybe_fail(name) ? -1 : unlink(name);
}

int cow_test_shm_unlink(const char *name)
{
#if COW_HAVE_SHM
    return maybe_fail(name) ? -1 : shm_unlink(name);
#else
    (void)name;
    errno = ENOTSUP;
    return -1;
#endif
}

int cow_test_shm_open(const char *name, int flags, mode_t mode)
{
#if COW_HAVE_SHM
    int fd = shm_open(name, flags, mode);
    if (fd >= 0) {
        assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        if (flags & O_CREAT)
            ++atomic_creations;
    }
    return fd;
#else
    (void)name;
    (void)flags;
    (void)mode;
    errno = ENOTSUP;
    return -1;
#endif
}

int cow_test_open(const char *name, int flags, ...)
{
    if (flags & O_CREAT) {
        va_list arguments;
        mode_t mode;
        int fd;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
        fd = open(name, flags, mode);
        if (fd >= 0) {
#ifdef O_CLOEXEC
            assert(flags & O_CLOEXEC);
#endif
            assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
            ++atomic_creations;
        }
        return fd;
    }
    return open(name, flags);
}

#if COW_HAVE_MKOSTEMP
int cow_test_mkostemp(char *name, int flags)
{
    int fd = mkostemp(name, flags);
    assert(flags & O_CLOEXEC);
    if (fd >= 0) {
        assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        ++atomic_creations;
    }
    return fd;
}
#endif

static int open_retained(int create)
{
    int flags = create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY;
#if TEST_SHM && COW_HAVE_SHM
    return shm_open(retained_name, flags, 0600);
#elif TEST_SHM
    (void)flags;
    errno = ENOTSUP;
    return -1;
#else
    return open(retained_name, flags, 0600);
#endif
}

static int unlink_retained(void)
{
#if TEST_SHM && COW_HAVE_SHM
    return shm_unlink(retained_name);
#elif TEST_SHM
    errno = ENOTSUP;
    return -1;
#else
    return unlink(retained_name);
#endif
}

int main(void)
{
    size_t page = cow_page_size();
    int fd;
    int inspect;
    unsigned before;
    assert(page);
    alarm(10);
#if TEST_SHM && !COW_HAVE_SHM
    puts("backing cleanup skipped: POSIX shared memory unavailable");
    return 77;
#endif
#if !TEST_SHM && !COW_HAVE_MKOSTEMP && !defined(O_CLOEXEC)
    puts("backing cleanup skipped: atomic close-on-exec disk creation unavailable");
    return 77;
#endif
    failures = 2;
    assert(cow_backing_create(page) == -1);
    inspect = open_retained(0);
    assert(inspect >= 0);
    close(inspect);
    /* A failed cleanup keeps its identity and descriptor for another try. */
    assert(cow_cleanup() == -1 && errno == EACCES);
    assert(cow_cleanup() == 0);
    assert(open_retained(0) == -1 && errno == ENOENT);

    /* A different object at a recycled name must survive our cleanup. */
    failures = 1;
    assert(cow_backing_create(page) == -1);
    assert(unlink_retained() == 0);
    fd = open_retained(1);
    assert(fd >= 0);
    assert(cow_cleanup() == 0);
    inspect = open_retained(0);
    assert(inspect >= 0);
    close(inspect);
    close(fd);
    assert(unlink_retained() == 0);

    /* EINTR is retried immediately and does not leave a queued object. */
    interrupted = 1;
    before = atomic_creations;
    fd = cow_backing_create(page);
    assert(fd >= 0);
    assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    close(fd);
    assert(atomic_creations == before + 1);
    assert(cow_cleanup() == 0);
    assert(open_retained(0) == -1 && errno == ENOENT);
    puts("backing cleanup: retries, object identity and atomic close-on-exec passed");
    return 0;
}
