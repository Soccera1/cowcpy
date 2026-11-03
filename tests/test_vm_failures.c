#define _GNU_SOURCE
#include "cowcpy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

static unsigned fixed_calls;
static int fail_fixed;
static int fail_preflight;

void *cow_test_mmap(void *address, size_t size, int protection, int flags,
                    int fd, off_t offset)
{
    if (flags & MAP_FIXED) {
        ++fixed_calls;
        if (fail_fixed) {
            --fail_fixed;
            /* Model a destructive mmap failure, including loss of the old
             * mapping. Recovery must restore all intended bytes. */
            assert(munmap(address, size) == 0);
            errno = ENOMEM;
            return MAP_FAILED;
        }
    } else if (fd >= 0 && fail_preflight) {
        --fail_preflight;
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return mmap(address, size, protection, flags, fd, offset);
}

int main(void)
{
    size_t size = 8 * cow_page_size();
    unsigned char *src = cow_alloc(size);
    unsigned char *dst = cow_alloc(size);
    unsigned char *clone;
    assert(src && dst);
    cow_set_min_copy(0);
    memset(src, 0x6b, size);
    if (cow_features() & COW_FEATURE_REMAP) {
        assert(cow_freeze(src) == 0);
        fail_fixed = 1;
        cow_memcpy(dst, src, size);
        assert(fail_fixed == 0 && fixed_calls >= 2);
        assert(memcmp(src, dst, size) == 0);
        assert(cow_thaw(src) == 0);
        src[0] = 9;
        assert(dst[0] == 0x6b);
        assert(cow_freeze(src) == 0);
        fail_preflight = 1;
        cow_memcpy(dst, src, size);
        assert(fail_preflight == 0 && memcmp(src, dst, size) == 0);
    }
    fixed_calls = 0;
    assert(cow_memcpy_checked(dst, src, size) == 0);
    assert(fixed_calls == 0);
    clone = cow_memdup(src, size);
    assert(clone && memcmp(clone, src, size) == 0);
    assert(fixed_calls == 0);
    assert(cow_free(clone) == 0);
    assert(cow_free(src) == 0);
    assert(cow_free(dst) == 0);
    puts("VM failures: destructive-failure recovery and non-destructive copy APIs passed");
    return 0;
}
