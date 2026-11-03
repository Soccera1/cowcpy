#define _POSIX_C_SOURCE 200809L
#include "cowcpy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t page;
static atomic_int stop_worker;
static atomic_int worker_ready;

static void *allocation_worker(void *unused)
{
    (void)unused;
    atomic_store(&worker_ready, 1);
    while (!atomic_load(&stop_worker)) {
        unsigned char *a = cow_alloc(8 * page);
        unsigned char *b = cow_alloc(8 * page);
        assert(a && b);
        memset(a, 71, 8 * page);
        cow_memcpy(b, a, 8 * page);
        assert(b[0] == 71);
        assert(cow_free(a) == 0 && cow_free(b) == 0);
    }
    return NULL;
}

static void custom_segv(int signal_number)
{
    (void)signal_number;
    _exit(99);
}

static void test_frozen_abort_sizes(unsigned char *frozen, const void *src,
                                    size_t size)
{
    const size_t counts[] = {1, size};
    size_t i;
    cow_set_min_copy(4 * page);
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        pid_t child = fork();
        int status;
        assert(child >= 0);
        if (!child) {
            struct rlimit limit = {0, 0};
            assert(setrlimit(RLIMIT_CORE, &limit) == 0);
            alarm(3);
            cow_memcpy(frozen, src, counts[i]);
            _exit(1);
        }
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    cow_set_min_copy(0);
}

static void test_signal_ownership(void)
{
    struct sigaction original;
    struct sigaction action;
    struct sigaction actual;
    unsigned char *a = cow_alloc(8 * page);
    unsigned char *b = cow_alloc(8 * page);
    assert(a && b);
    memset(&action, 0, sizeof(action));
    action.sa_handler = custom_segv;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGSEGV, &action, &original) == 0);
    memset(a, 29, 8 * page);
    cow_memcpy(b, a, 8 * page);
    assert(b[0] == 29);
    assert(sigaction(SIGSEGV, NULL, &actual) == 0);
    assert(actual.sa_handler == custom_segv);
    assert(sigaction(SIGSEGV, &original, NULL) == 0);
    assert(cow_free(a) == 0 && cow_free(b) == 0);
}

static void test_clone_and_freeze(void)
{
    unsigned char *src = cow_alloc(8 * page + 17);
    unsigned char *a;
    unsigned char *b;
    unsigned char *dst = cow_alloc(8 * page + 17);
    int frozen;
    assert(src && dst);
    memset(src, 0x5a, 8 * page + 17);
    frozen = cow_freeze(src) == 0;
    a = cow_memdup(src, 8 * page + 17);
    b = cow_memdup(src + 3, 6 * page + 7);
    assert(a && b);
    assert(memcmp(a, src, 8 * page + 17) == 0);
    assert(memcmp(b, src + 3, 6 * page + 7) == 0);
    cow_memcpy(dst, src, 8 * page + 17);
    assert(memcmp(dst, src, 8 * page + 17) == 0);
    if (frozen) {
        errno = 0;
        assert(cow_memcpy_checked(src, dst, page) == -1 && errno == EPERM);
        test_frozen_abort_sizes(src, dst, 8 * page);
        assert(cow_thaw(src) == 0);
    }
    src[0] = 0;
    a[page] = 1;
    b[2 * page] = 2;
    assert(a[0] == 0x5a && dst[0] == 0x5a);
    assert(src[page] == 0x5a && src[2 * page + 3] == 0x5a);
    assert(cow_free(src) == 0);
    assert(a[1] == 0x5a && b[1] == 0x5a);
    assert(cow_free(a) == 0);
    assert(cow_free(b) == 0);
    assert(cow_free(dst) == 0);
}

static void test_stable_buffers_and_checked(void)
{
    unsigned char *src = cow_alloc(8 * page);
    unsigned char *dst = cow_alloc_ex(8 * page, COW_ALLOC_EAGER);
    unsigned char ordinary[257];
    assert(src && dst);
    memset(src, 83, 8 * page);
    cow_memcpy(dst, src, 8 * page);
    assert(memcmp(dst, src, 8 * page) == 0);
    assert(cow_memcpy_checked(src + 1, dst + 2, page + 9) == 0);
    assert(cow_memcpy_checked(ordinary, src, sizeof(ordinary)) == 0);
    assert(ordinary[0] == 83 && ordinary[256] == 83);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
}

static void test_no_backing_resources(void)
{
    struct rlimit original;
    struct rlimit limited;
    unsigned char *src;
    unsigned char *dst;
    assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
    limited = original;
    limited.rlim_cur = 0;
    assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
    src = cow_alloc(8 * page);
    dst = cow_alloc(8 * page);
    assert(src && dst);
    memset(src, 0x7d, 8 * page);
    cow_memcpy(dst, src, 8 * page);
    assert(memcmp(dst, src, 8 * page) == 0);
    assert(cow_freeze(src) == -1);
    src[0] = 9;
    assert(dst[0] == 0x7d);
    assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
}

static void test_fork(void)
{
    unsigned char *src = cow_alloc(8 * page);
    unsigned char *dst = cow_alloc(8 * page);
    pid_t child;
    int status;
    assert(src && dst);
    memset(src, 0x3c, 8 * page);
    cow_memcpy(dst, src, 8 * page);
    child = fork();
    assert(child >= 0);
    if (!child) {
        unsigned char *fresh = cow_alloc(page);
        assert(fresh);
        src[0] = 1;
        dst[1] = 2;
        assert(src[1] == 0x3c && dst[0] == 0x3c);
        assert(cow_free(fresh) == 0);
        assert(cow_free(src) == 0);
        assert(cow_free(dst) == 0);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(src[0] == 0x3c && dst[1] == 0x3c);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
}

static void test_independent_pending(void)
{
    unsigned char *a = cow_alloc(2 * page);
    unsigned char *b = cow_alloc(2 * page);
    unsigned char *c = cow_alloc(2 * page);
    unsigned char *d = cow_alloc(2 * page);
    int fds[2];
    char byte;
    assert(a && b && c && d);
    memset(a, 21, 2 * page);
    memset(c, 43, 2 * page);
    cow_memcpy(b, a, 2 * page);
    cow_memcpy(d, c, 2 * page);
    assert(pipe(fds) == 0);
    errno = 0;
    assert(write(fds[1], b, 1) == -1 && errno == EFAULT);
    errno = 0;
    assert(write(fds[1], d, 1) == -1 && errno == EFAULT);
    assert(cow_materialize_range(b, 1) == 0);
    assert(write(fds[1], b, 1) == 1);
    assert(read(fds[0], &byte, 1) == 1 && byte == 21);
    errno = 0;
    assert(write(fds[1], d, 1) == -1 && errno == EFAULT);
    a[page] = 0;
    assert(b[page] == 21);
    assert(cow_free(a) == 0);
    assert(cow_free(b) == 0);
    assert(cow_free(c) == 0);
    assert(d[page] == 43);
    assert(cow_free(d) == 0);
    close(fds[0]);
    close(fds[1]);
}

static void test_fork_with_active_apis(void)
{
    pthread_t worker;
    pid_t child;
    int status;
    unsigned char *a = cow_alloc(page);
    assert(a);
    a[0] = 37;
    assert(pthread_create(&worker, NULL, allocation_worker, NULL) == 0);
    while (!atomic_load(&worker_ready))
        ;
    child = fork();
    assert(child >= 0);
    if (!child) {
        a[0] = 91;
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(a[0] == 37);
    atomic_store(&stop_worker, 1);
    assert(pthread_join(worker, NULL) == 0);
    assert(cow_free(a) == 0);
}

int main(void)
{
    alarm(30);
    page = cow_page_size();
    assert(page);
    cow_set_min_copy(0);
    test_stable_buffers_and_checked();
    test_no_backing_resources();
    if (cow_features() & COW_FEATURE_SIGNAL) {
        test_independent_pending();
    } else {
        test_signal_ownership();
        test_clone_and_freeze();
        test_fork();
        test_fork_with_active_apis();
    }
    printf("%s: clone, capabilities, fallback and process features passed\n", cow_backend());
    return 0;
}
