#define _POSIX_C_SOURCE 200809L
#include "cowcpy.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t page;
static int signal_backend;

static void fill(unsigned char *ptr, size_t size, unsigned seed)
{
    size_t i;
    for (i = 0; i < size; ++i)
        ptr[i] = (unsigned char)((i * 37 + i / 113 + seed) % 251);
}

static void test_alloc(void)
{
    unsigned char *ptr = cow_alloc(page + 7);
    size_t i;
    assert(ptr);
    assert((uintptr_t)ptr % page == 0);
    for (i = 0; i < page + 7; ++i)
        assert(ptr[i] == 0);
    errno = 0;
    assert(cow_free(ptr + 1) == -1 && errno == EINVAL);
    assert(cow_free(ptr) == 0);
    assert(cow_free(NULL) == 0);
    errno = 0;
    assert(cow_alloc(0) == NULL && errno == EINVAL);
    errno = 0;
    assert(cow_alloc(SIZE_MAX) == NULL && errno == ENOMEM);
}

static void test_fallback(void)
{
    const char src[] = "ordinary read-only source";
    char dst[sizeof(src)];
    unsigned char *managed = cow_alloc(page * 2);
    assert(managed);
    assert(cow_memcpy(dst, src, sizeof(src)) == dst);
    assert(memcmp(dst, src, sizeof(src)) == 0);
    cow_memcpy(managed, src, sizeof(src));
    assert(memcmp(managed, src, sizeof(src)) == 0);
    assert(cow_memcpy(dst, src, 0) == dst);
    assert(cow_free(managed) == 0);
}

static void test_independence(void)
{
    size_t size = 5 * page;
    unsigned char *src = cow_alloc(size);
    unsigned char *dst = cow_alloc(size);
    unsigned char *expected = malloc(size);
    size_t i;
    assert(src && dst && expected);
    fill(src, size, 9);
    memcpy(expected, src, size);
    errno = EDOM;
    assert(cow_memcpy(dst, src, size) == dst);
    assert(errno == EDOM);
    /* Source write must snapshot the destination before changing the source.
     * Destination-first write must preserve the source, including other bytes
     * on the faulting page. Reading a third page exercises read faults. */
    src[11] ^= 0xff;
    dst[page + 17] ^= 0xff;
    assert(dst[2 * page + 9] == expected[2 * page + 9]);
    src[2 * page + 9] ^= 0xff;
    dst[3 * page + 33] ^= 0xff;
    src[3 * page + 34] ^= 0xff;
    expected[page + 17] ^= 0xff;
    expected[3 * page + 33] ^= 0xff;
    for (i = 0; i < size; ++i)
        assert(dst[i] == expected[i]);
    assert(src[page + 17] == (unsigned char)(dst[page + 17] ^ 0xff));
    assert(cow_free(src) == 0);
    assert(memcmp(dst, expected, size) == 0);
    assert(cow_free(dst) == 0);
    free(expected);
}

static void test_alignment(void)
{
    size_t size = page * 5 + 31;
    unsigned char *src = cow_alloc(size);
    unsigned char *dst = cow_alloc(size);
    unsigned char *expected = malloc(size);
    const size_t offsets[] = {0, 1, 7, 0, 13, 1};
    size_t i;
    assert(src && dst && expected);
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        size_t src_offset = offsets[i];
        size_t dst_offset = i == 5 ? 2 : src_offset;
        size_t count = i == 3 ? page - 1 : page * 3 + 19;
        fill(src, size, (unsigned)i);
        memset(dst, 0xa5, size);
        memset(expected, 0xa5, size);
        memcpy(expected + dst_offset, src + src_offset, count);
        cow_memcpy(dst + dst_offset, src + src_offset, count);
        assert(memcmp(dst, expected, size) == 0);
        src[src_offset + 20] ^= 0xff;
        assert(memcmp(dst, expected, size) == 0);
    }
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
    free(expected);
}

static void test_chains(void)
{
    size_t size = 4 * page;
    unsigned char *a = cow_alloc(size);
    unsigned char *b = cow_alloc(size);
    unsigned char *c = cow_alloc(size);
    unsigned char *expected = malloc(size);
    unsigned iteration;
    assert(a && b && c && expected);
    fill(a, size, 27);
    for (iteration = 0; iteration < 40; ++iteration) {
        cow_memcpy(b, a, size);
        b[page + iteration] ^= 0xff;
        memcpy(expected, b, size);
        cow_memcpy(c, b, size);
        b[iteration] ^= 0x55;
        a[iteration] ^= 0x33;
        assert(memcmp(c, expected, size) == 0);
        cow_memcpy(a, c, size);
        assert(memcmp(a, expected, size) == 0);
    }
    /* Disjoint ranges of a single managed allocation are valid. */
    cow_memcpy(a + 2 * page, a, 2 * page);
    a[0] ^= 0xff;
    assert(a[2 * page] == (unsigned char)(a[0] ^ 0xff));
    assert(cow_free(a) == 0);
    assert(cow_free(b) == 0);
    assert(cow_free(c) == 0);
    free(expected);
}

static void test_free_pending(void)
{
    unsigned char *src = cow_alloc(3 * page);
    unsigned char *dst = cow_alloc(3 * page);
    assert(src && dst);
    memset(src, 73, 3 * page);
    cow_memcpy(dst, src, 3 * page);
    assert(cow_free(dst) == 0);
    src[page] = 91;
    dst = cow_alloc(3 * page);
    assert(dst);
    cow_memcpy(dst, src, 3 * page);
    assert(cow_free(src) == 0);
    assert(dst[0] == 73 && dst[page] == 91 && dst[2 * page] == 73);
    assert(cow_free(dst) == 0);
}

static void custom_handler(int sig)
{
    (void)sig;
    _exit(77);
}

static void test_existing_handler(void)
{
    pid_t child;
    int status;
    if (!signal_backend)
        return;
    child = fork();
    assert(child >= 0);
    if (!child) {
        struct sigaction action;
        struct sigaction actual;
        unsigned char *src = cow_alloc(page);
        unsigned char *dst = cow_alloc(page);
        assert(src && dst);
        memset(&action, 0, sizeof(action));
        action.sa_handler = custom_handler;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGSEGV, &action, NULL) == 0);
        memset(src, 17, page);
        cow_memcpy(dst, src, page);
        assert(dst[0] == 17);
        assert(sigaction(SIGSEGV, NULL, &actual) == 0);
        assert(actual.sa_handler == custom_handler);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_materialize_io(void)
{
    unsigned char *src = cow_alloc(page);
    unsigned char *dst = cow_alloc(page);
    int fds[2];
    char byte;
    assert(src && dst);
    src[0] = 99;
    cow_memcpy(dst, src, page);
    assert(pipe(fds) == 0);
    if (signal_backend) {
        /* A kernel copy does not invoke the userspace SIGSEGV handler. This
         * also proves that the test actually exercised the lazy backend. */
        errno = 0;
        assert(write(fds[1], dst, 1) == -1 && errno == EFAULT);
    }
    assert(cow_materialize() == 0);
    assert(write(fds[1], dst, 1) == 1);
    assert(read(fds[0], &byte, 1) == 1 && byte == 99);
    close(fds[0]);
    close(fds[1]);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
}

static void test_unrelated_signal(void)
{
    pid_t child;
    int status;
    if (!signal_backend)
        return;
    child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {0, 0};
        unsigned char *src = cow_alloc(page);
        unsigned char *dst = cow_alloc(page);
        assert(src && dst);
        assert(setrlimit(RLIMIT_CORE, &limit) == 0);
        cow_memcpy(dst, src, page);
        raise(SIGSEGV);
        _exit(1);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

#if defined(__linux__)
static char mapping_type(const void *ptr)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    char result = '?';
    assert(maps);
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start, end;
        char permissions[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &end, permissions) == 3 &&
            (uintptr_t)ptr >= start && (uintptr_t)ptr < end) {
            assert(strstr(line, "memfd:cowcpy"));
            result = permissions[3];
            break;
        }
    }
    fclose(maps);
    return result;
}

static void test_linux_mappings(void)
{
    unsigned char *src;
    unsigned char *dst;
    if (strcmp(cow_backend(), "memfd") != 0)
        return;
    src = cow_alloc(page);
    dst = cow_alloc(page);
    assert(src && dst);
    memset(src, 42, page);
    assert(mapping_type(src) == 's');
    cow_memcpy(dst, src, page);
    assert(mapping_type(src) == 'p');
    assert(mapping_type(dst) == 'p');
    assert(dst[0] == 42);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
}
#endif

int main(void)
{
    page = cow_page_size();
    assert(page > 0);
    cow_set_min_copy(0);
    signal_backend = strcmp(cow_backend(), "signal") == 0;
    /* Must run before the process installs the library handler. */
    test_existing_handler();
    test_alloc();
    test_fallback();
    test_independence();
    test_alignment();
    test_chains();
    test_free_pending();
    test_materialize_io();
    test_unrelated_signal();
#if defined(__linux__)
    test_linux_mappings();
#endif
    printf("%s backend: all tests passed\n", cow_backend());
    return 0;
}
