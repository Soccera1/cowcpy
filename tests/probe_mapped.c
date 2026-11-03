#define _GNU_SOURCE
#include "cowcpy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned replacements;
void *cow_test_mmap(void *address, size_t size, int protection, int flags,
                    int fd, off_t offset)
{
    if (flags & MAP_FIXED)
        ++replacements;
    return mmap(address, size, protection, flags, fd, offset);
}

int main(void)
{
    size_t size = 2 * cow_page_size();
    unsigned char *src;
    unsigned char *dst;
    alarm(10);
    if (!(cow_features() & COW_FEATURE_REMAP))
        return 1;
    cow_set_min_copy(0);
    src = cow_alloc(size);
    dst = cow_alloc(size);
    assert(src && dst);
    memset(src, 0x4b, size);
    cow_memcpy(dst, src, size);
    assert(replacements >= 2);
    assert(memcmp(src, dst, size) == 0);
    src[0] = 1;
    dst[1] = 2;
    assert(dst[0] == 0x4b && src[1] == 0x4b);
    assert(cow_free(src) == 0 && cow_free(dst) == 0);
    return 0;
}
