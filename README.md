# cowcpy

A memcpy-compatible copy library for POSIX programs, with page-based
copy-on-write optimizations for large buffers. Small copies and unsupported
cases use libc `memcpy`.

Ordinary buffers work with `cow_memcpy`. Allocate with `cow_alloc` to make
buffers eligible for page optimizations. As with `memcpy`, ranges must be
valid and non-overlapping.

With mutable-source remapping enabled, exclude concurrent source readers
during copying. Use `MUTABLE_SNAPSHOTS=0` or freeze the source when readers
must run concurrently. `cow_memcpy` aborts on library-detected invalid managed
destinations or unrecoverable remapping failures. Use `cow_memcpy_checked`
to receive library errors through its return value instead.

## Build

```sh
make
make check
./build/example
```

The default build produces `build/libcowcpy.a` and `build/libcowcpy.so`.
It detects available backing APIs and tests private mapping behavior.
Linux prefers memfd backing; other POSIX systems can use POSIX shared memory.
Apple builds prefer the Mach VM copy API when available and working.
Signal handling is an explicit opt-in.

The implementation uses C11 and pthreads. The public header works with C89,
C99, and C++. Compile the library separately and link your application:

```sh
cc -std=c89 -Iinclude app.c build/libcowcpy.a -pthread -o app
```

Some older systems also need `-lrt`; detected extra libraries are recorded
in `build/cowcpy.libs`. Install both libraries and the header with
`make install PREFIX=/your/prefix`; `DESTDIR` is supported.
Installation also provides `lib/pkgconfig/cowcpy.pc` for pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs cowcpy) -o app
```

Use `pkg-config --static --libs cowcpy` for static linking dependencies.
For a custom prefix, add its `lib/pkgconfig` directory to `PKG_CONFIG_PATH`.
The package version defaults to `0.1.0` and can be set with `make VERSION=...`.

## Example

```c
#include <cowcpy.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    size_t size = 16 * cow_page_size();
    unsigned char *src = cow_alloc(size);
    unsigned char *dst = cow_alloc(size);

    if (!src || !dst) {
        perror("cow_alloc");
        cow_free(src);
        cow_free(dst);
        return 1;
    }

    memset(src, 42, size);
    cow_memcpy(dst, src, size);
    src[0] = 7;
    printf("source: %u; copy: %u\n", (unsigned)src[0], (unsigned)dst[0]);
    /* Prints: source: 7; copy: 42 */

    cow_free(src);
    cow_free(dst);
    return 0;
}
```

Use `cow_free` for managed allocations. Either copy can be freed first.
Libc `free` and `realloc` cannot be used on managed buffers.

For repeated snapshots of unchanged data, call `cow_freeze(src)` first.
It makes the allocation read-only and caches a backing that later copies
can reuse. Call `cow_thaw(src)` before modifying it. Check both return values.

## API

Include [cowcpy.h](include/cowcpy.h).

| Function | Purpose |
| --- | --- |
| `cow_alloc(n)` | Page-aligned, zero-filled storage; NULL with `errno` on failure. Zero size is rejected. |
| `cow_alloc_ex(n, COW_ALLOC_EAGER)` | Storage whose mapping is kept stable during copies, for pinned or registered buffers. |
| `cow_free(p)` | Release an allocation; accepts NULL. Returns 0 or -1 with `errno`. |
| `cow_memcpy(dst, src, n)` | Copy and return `dst`, preserving `errno`. |
| `cow_memcpy_checked(dst, src, n)` | Copy without destructive fixed remapping; returns 0 or -1 with `errno`. May use eager copying. |
| `cow_memdup(src, n)` | Allocate an independent copy without replacing any existing mapping. Returns NULL with `errno` on failure. |
| `cow_freeze(p)`, `cow_thaw(p)` | Cache an immutable source / make it writable again. Require the original allocation pointer; return 0 or -1. |
| `cow_materialize()` | Complete all pending signal-backend copies. |
| `cow_materialize_range(p, n)` | Complete pending copies touching a range. Both materialization functions return 0 or -1. |
| `cow_set_min_copy(n)` | Set the minimum byte count for page optimizations. Default: four host pages. |
| `cow_page_size()` | Host page size, or 0 on initialization failure. |
| `cow_backend()` | Selected backend: `mapped`, `mach`, `signal`, or `eager`. |
| `cow_features()` | Runtime feature bits defined in the public header. |
| `cow_cleanup()` | Retry retained backing-name cleanup; returns 0 or -1 with `errno`. Also attempted at normal exit. |

## Feature selection

```sh
make BACKEND=auto                # Prefer native APIs; portable mapping fallback
make BACKING=shm                 # Use POSIX shared memory instead of memfd
make REMAP=no                    # Disable fixed-address mapping replacement
make MUTABLE_SNAPSHOTS=0         # Optimize frozen sources; eagerly copy mutable ones
make MIN_COPY_PAGES=16           # Raise the optimization threshold
make BACKEND=eager               # Use libc copies throughout
make BACKEND=signal              # Opt into the tested synchronous-fault extension
make BACKEND=signal SIGNAL_MODE=single  # Direct filling; single-threaded access
make BACKING=tmp                 # Explicitly permit disk backing; respects TMPDIR
make RUN_PROBES=0                # Cross-compile without running target binaries
```

`BACKEND` also accepts `mapped` and `mach`. `BACKING` also accepts `auto`,
`memfd`, and `anonymous`. `REMAP` accepts `auto`, `yes`, and `no`.
`ALLOW_DISK=1` permits disk backing as a last fallback; the default is 0.
Cross-compilation conservatively disables untested fixed remaps and automatic
signal activation; explicit overrides remain available.

Options regenerate `build/cowcpy_config.h` and rebuild affected objects.
Backing or snapshot failures at runtime fall back to eager copying where
possible. A working optimization is not a promise that it will outperform
`memcpy`; tune the threshold and snapshot policy for your workload.

## Platform contract

The default backends use private mappings, preserve independent data across
`fork`, and leave signal dispositions alone. Independent buffers use separate
mutexes. Callers still synchronize conflicting buffer access and buffer
lifetime. POSIX restrictions on the child of a multithreaded `fork` still apply.

The signal backend needs host extensions beyond strict POSIX, reserves an
extra virtual mapping, and has additional signal, I/O, and fork restrictions.
Fixed-address replacement also has an unavoidable catastrophic-failure case.
Use `REMAP=no`, checked copies, or `cow_memdup` when that risk is unacceptable.

See [platform behavior and tradeoffs](docs/portability.md) for backend details,
performance costs, failure recovery, and supported integration patterns.
