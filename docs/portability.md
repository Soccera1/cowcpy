# Platform behavior and tradeoffs

## Choosing a backend

`BACKEND=auto` prefers `mach_vm_copy` on Apple targets with the API available.
The library checks its basic copy isolation at initialization and falls back
if the API fails. Other targets use private file mappings when a backing can
be created, or eager copying otherwise. Native Mach behavior still needs
validation on Apple hardware; development tests run on Linux.

Backing selection is independent: `BACKING=auto` tries Linux `memfd_create`,
then POSIX `shm_open`. Objects are normally unlinked immediately. If removal
fails, the library retains the name and original descriptor for identity-checked
cleanup retries. It retries interrupted removals immediately and other failures
before creating another backing, through `cow_cleanup`, and at normal exit.
Persistent permission or filesystem errors can still leave named objects;
`cow_cleanup` reports them instead of discarding the cleanup state. Retrying
cleanup requires exclusive ownership of the generated names; other code must
not concurrently replace objects at those names.

POSIX shared-memory descriptors are close-on-exec at creation. Disk backing
uses detected `mkostemp(O_CLOEXEC)`, or exclusive `open(O_CLOEXEC)` when that
extension is unavailable. Hosts without atomic close-on-exec disk creation
use eager fallback. Ordinary allocations are private anonymous mappings, so backing
restrictions do not prevent allocation. If a snapshot cannot create or fill
its backing, copying falls back to `memcpy`. Disk files are used only with
`BACKING=tmp` or `ALLOW_DISK=1`; `TMPDIR` controls their location.

Build probes detect available interfaces, extra link libraries, and basic
private-mapping/write-isolation behavior. They test functionality, not
performance or every possible execution. Runtime checks and fallbacks also
handle a binary running under a different kernel, sandbox, or resource limit.
With `RUN_PROBES=0`, automatic fixed replacement is disabled. Use `REMAP=yes`
only when the target platform's mapping behavior is known.

`cow_backend()` reports the selected family; `cow_features()` reports enabled
operations. Feature bits do not promise that every attempt succeeds: quotas,
descriptor limits, or memory pressure can still require fallback or an error.

## Large copies and snapshots

The default threshold is four host pages. Smaller copies use libc without
page remapping; managed destinations still receive validity/frozen checks.
The mapped backend optimizes complete interior pages only when both addresses
have matching page offsets and belong to eligible managed allocations. Partial
pages, different offsets, and ordinary buffers use eager copying. The Mach
backend can use an ordinary source with an eligible managed destination.

For a mutable source, the mapped backend captures current bytes with `pwrite`,
then maps the immutable backing privately into both buffers. This shares
physical backing after creation but still incurs an initial eager data copy.
It favors memory sharing over raw copy latency. `MUTABLE_SNAPSHOTS=0` disables
that tradeoff; mutable sources then use `memcpy`.

`cow_freeze` makes an allocation read-only and caches one snapshot. Repeated
copies and aligned `cow_memdup` calls reuse that backing without recapturing
the bytes. The original allocation and cache can occupy separate physical
storage. `cow_thaw` drops the cache and permits writes; existing copies retain
their own private views. Freeze/thaw require exclusive access and are not
supported by the optional signal backend.

`cow_memdup` maps its result at a new address, so it avoids fixed-address
replacement. For ordinary sources it first captures the bytes; small clones
and unavailable backing use allocation plus `memcpy`. It does not modify or
remap the source. Callers must limit accesses to the requested allocation size.
Page rounding may leave extra bytes hardware-accessible in the last page;
the allocator does not provide guard pages or redzones.

## Threads and fork

Mapped and native API operations pin allocation metadata briefly under a
registry mutex, then lock the involved allocations in address order. Copies
between unrelated buffers can proceed concurrently. Freeing waits for
in-flight API references. Callers must still prevent concurrent buffer use
after free and conflicting reads/writes, just as with ordinary C memory.

Mutable-source fixed remapping requires excluding other access to the source
pages during copying. Frozen-source copies leave the source mapping intact,
so ordinary concurrent source reads are allowed. Use `MUTABLE_SNAPSHOTS=0`
when mutable buffers may have concurrent readers. Destination access requires
the usual copy-time exclusion. Freeze/thaw affect the entire allocation.

Default backends always expose private mappings. At-fork hooks wait for API
operations to complete and keep the registry consistent. Parent and child
buffer contents remain independent. This does not lift POSIX's restrictions
on calling non-async-signal-safe functions before exec after a multithreaded
fork. The signal backend does not advertise `COW_FEATURE_FORK`.

There are no global busy-wait locks. Faults in the opt-in signal backend still
serialize through its mutex. Page faults, backing creation, and kernel locks
have unbounded latency; this is not a hard-real-time memory primitive.

## I/O and stable mappings

Default backends need no materialization before ordinary system calls. The
signal backend requires `cow_materialize_range` before kernel I/O because
kernel buffer access can return `EFAULT` without delivering a user handler.

Allocate pinned, GPU, DMA, or RDMA buffers with `COW_ALLOC_EAGER`, or keep them
outside the managed allocator. EAGER excludes both source and destination
from copy-time remapping and protection changes. It does not register, pin,
or otherwise make the buffer compatible with a particular device API.
Perform registration yourself and keep the allocation alive. Do not freeze
registered writable buffers or change managed mappings independently.

## Failure recovery

Allocation, clone, freeze/thaw, checked-copy, and materialization APIs expose
errors through their documented return values and `errno`. Checked copies
avoid destructive fixed replacement; they use eager copying when necessary.
They still require valid memory and do not catch arbitrary invalid pointers.

Normal mapped copies preflight a replacement through an independent mapping.
If fixed replacement fails, the library releases the preflight reservation
and attempts to restore the intended snapshot at the original address. Tests
inject failures that remove the old mapping to exercise this recovery.

POSIX does not provide an infallible mapping-replacement transaction. If both
replacement and recovery fail, the original address may be unusable and
`cow_memcpy` terminates the process. Writing to a frozen destination is also
invalid; checked copies report `EPERM` instead. `REMAP=no` disables this
replacement path globally; `cow_memcpy_checked` and `cow_memdup` avoid it per
operation. Ordinary out-of-memory faults and device/kernel failures remain
outside the guarantees of a memcpy-shaped API.

There is no portable query that establishes that a failed replacement left
the original mapping, backing identity, and access protections intact.
Residency information alone would not prove this. A double replacement failure
therefore remains conservative, even on a host that may have preserved the
original mapping. Checked copies provide a non-aborting channel for detected
library errors; they still require valid caller memory.

## Optional synchronous-fault backend

`BACKEND=signal` explicitly opts into host support beyond POSIX. Its build
probe checks synchronized publication and delayed faults with retired
metadata. Without a runnable probe, select `SIGNAL_MODE=alias` explicitly to
enable it; otherwise it falls back to eager copying. Tests cannot certify
the safety of the direct-fill algorithm, so `SIGNAL_MODE=single` is always
an explicit single-threaded choice.

The alias path protects source pages read-only and destination pages
inaccessible, fills through a hidden writable mapping, then publishes a
completed page. Independent deferred copies coexist; only overlapping
dependencies are materialized before another copy. This is deferred copying,
not physical page sharing between source and destination.

The handler uses a mutex and `mprotect` for controlled synchronous faults.
Strict POSIX does not guarantee these operations inside a signal handler.
This path must be validated on its target OS. Asynchronous callbacks must
not call the library or access managed buffers. Existing SIGSEGV handlers
cause eager fallback; do not replace or block the library's handler while
copies are pending. Permission-restoration failures in a fault are fatal.
Only pages with a recorded library retirement generation qualify for a delayed
fault retry. Repeated faults on that page/generation fail with the default
signal disposition. Faults inside a library critical section also fail closed
instead of attempting to lock the same mutex recursively. Managed allocations
are not a blanket exemption from genuine protection faults.

Hidden aliases consume additional virtual addresses and shared backing.
Do not use live signal-backend allocations across fork: their shared public
mappings can expose parent/child writes. Choose the default backend when
fork, signal-runtime integration, or broad POSIX portability is required.
