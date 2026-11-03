#include "cowcpy.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    size_t size = 4 * cow_page_size();
    unsigned char *src = cow_alloc(size);
    unsigned char *dst = cow_alloc(size);
    if (!src || !dst) {
        perror("cow_alloc");
        cow_free(src);
        cow_free(dst);
        return 1;
    }
    memset(src, 'A', size);
    cow_memcpy(dst, src, size);
    src[0] = 'B';
    dst[size - 1] = 'C';
    printf("%s: source=%c%c, copy=%c%c\n", cow_backend(),
           src[0], src[size - 1], dst[0], dst[size - 1]);
    if (cow_free(src) < 0 || cow_free(dst) < 0) {
        perror("cow_free");
        return 1;
    }
    return 0;
}
