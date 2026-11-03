#ifndef COWCPY_H
#define COWCPY_H

#include <stddef.h>

#define COW_ALLOC_EAGER 1u
#define COW_FEATURE_CHECKED 1ul
#define COW_FEATURE_MEMDUP 2ul
#define COW_FEATURE_FORK 4ul
#define COW_FEATURE_FREEZE 8ul
#define COW_FEATURE_NATIVE 16ul
#define COW_FEATURE_REMAP 32ul
#define COW_FEATURE_SIGNAL 64ul

#ifdef __cplusplus
extern "C" {
#endif

/* Page-aligned, zero-filled storage. Zero size is rejected with EINVAL.
 * Release only with cow_free(), using the original allocation address. */
void *cow_alloc(size_t size);
/* EAGER allocations keep a stable mapping for pinned/registered I/O users. */
void *cow_alloc_ex(size_t size, unsigned flags);
int cow_free(void *ptr);

/* memcpy semantics, including undefined behavior for overlapping ranges.
 * Large managed buffers may use page optimizations. Small copies and
 * ordinary buffers use memcpy. Frozen buffers are valid read-only sources.
 * API calls are internally synchronized. Callers must synchronize conflicting
 * buffer accesses; see README.md for page-level, signal, and I/O restrictions.
 * Mutable-source remapping additionally excludes concurrent source readers;
 * use MUTABLE_SNAPSHOTS=0 or freeze the source to permit concurrent readers.
 * This function aborts on a frozen/poisoned managed destination or an
 * unrecoverable remap. Use cow_memcpy_checked for a non-aborting copy API.
 * SIGNAL_MODE=single builds require single-threaded deferred buffer access. */
void *cow_memcpy(void *dst, const void *src, size_t size);
/* Avoid destructive fixed remapping. Returns 0 or -1 with errno. */
int cow_memcpy_checked(void *dst, const void *src, size_t size);
/* Allocate an independent copy without replacing any existing mapping. */
void *cow_memdup(const void *src, size_t size);
/* Explicit immutability allows repeated copies to reuse a cached backing.
 * Freeze/thaw require the original allocation pointer and exclusive access. */
int cow_freeze(void *ptr);
int cow_thaw(void *ptr);
/* Minimum size at which page optimizations are considered. */
void cow_set_min_copy(size_t size);
unsigned long cow_features(void);
/* Retry unlinking backing names retained after an earlier cleanup failure.
 * Returns 0 if none remain, or -1 with errno. Also attempted at normal exit. */
int cow_cleanup(void);

/* Resolve all deferred copies before kernel I/O. This may run concurrently
 * with valid buffer access. For the default backends it is a no-op. */
int cow_materialize(void);
int cow_materialize_range(const void *ptr, size_t size);
size_t cow_page_size(void);
const char *cow_backend(void);

#ifdef __cplusplus
}
#endif
#endif
