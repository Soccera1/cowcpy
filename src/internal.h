#ifndef COWCPY_INTERNAL_H
#define COWCPY_INTERNAL_H

#include <stddef.h>
void *cow_signal_alloc(size_t size);
void *cow_signal_alloc_ex(size_t size, unsigned flags);
int cow_signal_free(void *ptr);
void *cow_signal_memcpy(void *dst, const void *src, size_t size);
int cow_signal_materialize(void);
int cow_signal_materialize_range(const void *ptr, size_t size);
size_t cow_signal_page_size(void);
const char *cow_signal_backend(void);
int cow_backing_create(size_t size);

#endif
