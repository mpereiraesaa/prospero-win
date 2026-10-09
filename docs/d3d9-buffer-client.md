# PE32 buffer staging

The client helper returns owned VirtualAlloc memory whose complete span fits
below 4 GiB. A process-wide 64 MiB budget bounds active staging across resources.
The proxy owns and serializes each helper state, retains its object identity and
provides the synchronous session call plus cancellation callback. This helper
does not implement COM or transport framing.

Lock first obtains the actual backend result and transfer generation, reserves
budget, allocates staging and copies all initial bytes through bounded reads.
Even DISCARD is prefilled because the backend may ignore it for pool, flags or
device-state reasons. The caller may modify part of the returned allocation.
Unlock publishes the entire writable span as owned chunks before real Unlock;
READONLY does not upload. Staging is freed only after transfer completion.

Allocation/read/write failures cancel the real backend lock and free staging.
Malformed replies or unsuccessful cleanup cancel the session so its teardown
owns any remaining native mapping. Original HRESULTs remain visible; cleanup
results are retained separately. Resource destruction must call client_cancel
before releasing its service registry reference. Nested client locks are
explicitly unsupported, matching the initial native adapter policy.

The PE32 lab uses real Wine VirtualAlloc and a controlled transport fixture. It
checks low-address spans, partial edits, read-only locks, read/write errors,
corrupt generations, cleanup and two-resource global budget exhaustion. The
separate native resource lab covers real DXVK execution; this controlled test
makes no backend or end-to-end COM proxy claim.

The allocation implementation lives in `pw_d3d9_staging.[ch]` so buffer and
texture helpers share the same atomic64MiB process-wide budget. Its internal
free API requires the original successful allocation pointer and length.
The existing PE32 fault/budget fixture also runs against this shared allocator.
