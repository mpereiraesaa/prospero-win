# Shared mutex backend

## Event and semaphore extension under development

Patch 0885 adds atomic state primitives for a subsequent event/semaphore
backend. It activates no runtime path, exports no function and changes no
mutex ABI or default. It is the first dependency of the extension, rather
than evidence of an event/semaphore performance improvement.

Each aligned cell has an immutable kind and maximum, a count/state, and a
SLOW bit. An auto-reset event consumes its signaled state on a successful
wait; a manual-reset event keeps it. Even the manual event's unchanged state
requires a compare-and-swap, so a concurrent server freeze rejects the fast
operation. Event set/reset and semaphore release return their previous state
only on success. Semaphore release respects the maximum without arithmetic
overflow. Empty waits, invalid requests, maximum violations, contention and
SLOW state fall back without changing the caller-local output.

Successful waits use acquire ordering; set/release operations use release
ordering. Freeze adopts exactly one atomic snapshot. Recursive slow entry
preserves the legacy state, and invalid legacy values remain slow. A native
hot operation attempts one strong compare-and-swap, with ordinary server
fallback on a race. These primitives accept native cells and local outputs;
they do not validate application pointers or handle permissions.

`python3 tests/test_wine_shared_sync_word.py` compiles the exact header from
the patch. Its bounded native fixture checks an independent transition
matrix for both event kinds and semaphore maxima 1–7, the maximum count
boundary, unchanged failure outputs, recursive freeze, stale CAS rejection,
four-thread count conservation across 3,000 server authority transfers, and
2,000 payload handoffs through auto-reset events. It does not start Wine or
exercise real handles, waits, APCs or object lifetime.

Patch 0886 supplies server freeze/adopt hooks for queue insertion and
removal, signaled/satisfied callbacks, queries, event set/reset/pulse and
semaphore release. Its native metadata keeps a sync-object reference while
a cell is active. It reuses the existing mutex request-depth, readiness and
handle alias hooks, preserving the mutex ABI. Patch 0886 alone has no
runtime client; patch 0887 supplies the opt-in client below.

Publication occurs only after the outermost server operation, with no
remaining waiters or debug/module disable. Cold preparation admits only
unnamed, non-inheritable, single-handle event/semaphore objects with the
ordinary server sync type. Events associated with kernel objects stay slow;
requesting the kernel-object list retires any existing cell before an
association can be added. Aliases, global handles, closes and backend disable
freeze/adopt state, remove publication entries and drop the activation
reference. Retired cells remain addressable and permanently slow, with their
sync pointers cleared. Their sync remains permanently ineligible, including
after readiness returns. Ordinary multiwait and SignalObjectAndWait still
use their existing callbacks and handlers; publication waits for nested
completions to finish.

The process retains at most one MiB of requested event/semaphore cell payload
over its lifetime. The limit is computed from the complete native cell size;
allocator metadata is additional. Only successful allocations consume the
budget. Retiring a cell never returns that allowance, since old native cache
entries must continue to address the same permanently SLOW word. An existing
binding remains usable at the limit. New objects rejected by the full budget
stay permanently on the ordinary path and produce exact negative client cache
entries, avoiding repeated cold allocation attempts. A transient allocation
failure leaves the object eligible for a later retry. This budget does not
change the mutex backend, ABI or event/semaphore default-off selection.

`python3 tests/test_wine_shared_sync_server.py` compiles the exact added
authority, cold preparation, queue and retirement bodies with native fixture
list/refcount callbacks. It checks 600 fast/legacy authority cycles, repeated
slow entry, nested request completion, persistent waiters, state adoption,
cold exclusions and pin/retirement lifetime. It positions a fixture-only
admission counter at the boundary, without allocating to a resource limit,
and checks allocation-failure retry, existing bindings at capacity, event and
semaphore ordinary fallback, unchanged state/output/refcounts, and no budget
reuse after retirement. This fixture does not establish
real Wine wait, APC or handle semantics.

Patch 0887 adds an independent `pw_wineserver_sync_backend` versioned native
ABI and exact paged handle cache. Its discovery validates version, structure,
word and pointer sizes and both required pointers. Cold lookup uses the
existing server lock, thread/TEB and pending-request gates, preserving thread
and global errors. Native caller-local word/access outputs change only on a
ready result. Invalid handles, allocation failures and unavailable server
contexts retry; valid permanently ineligible handles have exact negative
slots. Cache fills and all four existing close invalidations use
`fd_cache_mutex`. Cell and page storage remains addressable through teardown;
retired cells cannot be rebound to a new handle.

The independent `WINE_PS5_SYNC_SHARED=1` or prefix-local `pw_sync_shared`
containing exactly `1` with an optional newline enables discovery. Explicit
environment settings win; absent, malformed and unreadable settings stay
off. The shared and typed mutex selections and their existing ABI remain
unchanged. Disabling direct server calls prevents discovery. Non-in-process
builds return ordinary fallback from the same client entry point.

Warm single-object non-alertable waits require cached `SYNCHRONIZE` access;
event set/reset and semaphore release require their modify-state access.
Operations attempt one word CAS, with no new server lock or signal section
on a warm success. Empty waits, wrong types, invalid counts, maximum
violations, contention, SLOW state and readiness downgrade fall back without
changing output storage. The caller copies a successful local previous
state/count to the application only after the helper returns. Alertable and
multi-object waits, pulses, queries and SignalObjectAndWait retain ordinary
Wine behavior and the server authority hooks from 0886.

`python3 tests/test_wine_shared_sync_client.py` compiles the actual added ABI,
switch, client cache and operation bodies, with bounded native metadata. It
checks ABI mismatches, 15 server-context gates, all wait/modify permission
combinations for each kind, event previous states, semaphore limits,
36,000 warm operations without extra cold lookups or locks, 80 exact negative
handles, invalid-handle/allocation retries, a second cache page, close/reuse,
readiness/SLOW fallback and 12,000 protected semaphore sections across four
live threads. It also compiles the ordinary non-in-process fallback wrapper.
These tests do not run Wine or establish APC, signal, exception, wait-queue
integration or console performance. The real-Wine console semantic matrix,
matching module pair, HL2/load/city and 600-second stability gates remain
required before runtime acceptance.

The console owner reports the bounded matching pair (server `3cfaa720`,
ntdll `ddc5bc0e`) passed the ordinary 32-bit sync fixture with the switch
off and on: 12 cases, 330 checks, zero failures and Wine-exit in each arm.
HL2 measured 59.38 FPS with Wine-exit in both arms on the same console.
The reported 480-second city comparisons were 52.3/45.4 FPS (mean/minimum)
with the switch on, 50.8/46.5 off, and 53.0/47.2 on again. Verification of
the matching 200–440-second window and the 600-second stability gate is
pending. These reports do not establish the full 58/50 FPS target or an
accepted event/semaphore gain; selection remains default off.

Status: experimental Unix client/server backend implemented, build-tested
and measured on the console. Patch 0880 selects it by default for compatible
direct-call modules after console acceptance of the combined runtime pair.
Patch 0810 supplies server authority/lifetime hooks; 0820 adds the native ABI,
client cache and the original default-off switch. Native fixtures and SDK
pair builds pass. The original matching pair has completed ordinary 480-second and
600-second console runs. The ordinary 32-bit Wine fixture passes on the
default pair and on the candidate pair with the switch off and on. Broader
asynchronous thread/signal contracts remain unproven; the ordinary fixtures
and gameplay runs do not establish those contracts.

The performance target remains the full fixed GTA IV route at 1920×1080,
60 Hz, profiling off, average at least 58 FPS and minimum at least 50 FPS,
with the existing HL2 timedemo/clean-exit and load gates. The owner's latest
console policy (2026-10-04) sets stability to a 600-second run and ordinary
A/B runs to a 480-second script with a matched 200–440-second window.
This replaces the earlier 1,800-second stability requirement; it does not
change the FPS target. A helper test does not replace these gates.

## Console observations (2026-10-04)

Full capture audits use the same 200–440-second window. These are
equal-weight approximate FPS samples, not a frame-time-weighted average
or an instantaneous minimum. The console owner reports matching goal
settings and translator between the control and shared-backend runs.

| Run | Sample count | Mean approximate FPS | Minimum sampled FPS |
| --- | ---: | ---: | ---: |
| Default pair control | 158 | 32.68 | 27.42 |
| Original shared pair, 480-second script | 159 | 48.58 | 41.64 |
| Original shared pair, 600-second script | 159 | 49.07 | 41.11 |

The longer shared capture spans 602 seconds, records no file-limit failure
and follows the Wine-exit path. Its complete 460–600-second window has
92 samples, averaging 48.17 FPS with a minimum sampled value of 45.30.
The earlier control and 480-second shared run ended by `close-timeout`;
those endings do not establish a clean Wine exit.

These observations repeat the improvement but still miss the 58/50 FPS
target. The console owner supplied matching module/settings receipts. The
same ordinary 32-bit PE fixture passes all 11 cases and 174 checks with
zero failures in each arm: default modules, candidate modules with the
switch off, and candidate modules with the switch on. All three fixture
captures follow the Wine-exit path. Coverage includes recursion, access
fallbacks, multiwaits, SignalObjectAndWait, aliases, handle reuse, queued
handoff and normal-thread-exit abandonment; it does not cover every
asynchronous signal, APC, exception or forced-termination contract.

The matched load proxy (the first recorded memory allocation above
1.4 GB) occurs at about 94.7 seconds in all three gameplay captures. This
allocation marker is a proxy for loading, not a direct measurement of every
loading operation. The console owner reports HL2 timedemo results of
59.84 FPS with the shared switch off and 59.81 FPS with it on, both with
Wine-exit. Later cold-admission, retained-cell-cap and dump changes were
not part of these measured runs; they need separate review and comparisons.

### Combined default-on runtime acceptance

The console owner accepted the combined default-on pair on 2026-10-04:
server `0633d7c8` and NTDLL `853070a1`. It includes shared mutex selection,
image-view descriptor cleanup and the one-file `/dev/null` wrapper. The
600-second city run also uses the signed-bit-offset/high-byte translator
and cached computer-name DLLs. Its 200–440-second window averages 53.1
approximate FPS with a minimum sampled value of 45.4; the 460–600-second
window averages 52.7 with a minimum of 50.2. No file-limit error was reported,
and the same allocation-based load proxy remains 94.7 seconds. Sample counts
were not supplied in this receipt. The full 58/50 FPS target remains unmet.

The ordinary mutex PE v2 fixture passes 11 cases and 174 checks on the
combined pair with Wine-exit. The ordinary image-section PE fixture passes
six cases and 147 checks on both the reference and combined pairs, also
with Wine-exit. HL2 reports 59.36 FPS with Wine-exit against the same-console
reference's 59.38. These are owner-reported console receipts; they validate
the accepted combined configuration, rather than attributing its timing
to an individual default switch or to the translator alone.

The corresponding changes merged as #354 and #355. The later event/semaphore
extension, its retained-cell budget and profiler clock-gating candidate are
not part of these accepted modules or measurements.

## Ownership and mode changes

A permanent, separately aligned 64-byte cell has one atomic word: a unique
32-bit thread token, a 31-bit recursion count, and the high SLOW bit. Tokens
must not be reused. Cells remain readable after retirement and cannot be
reassigned to another mutex while a client might still address them.

There is one ownership authority at a time:

- With SLOW clear, the word is authoritative. Legacy owner/count fields and
  the owner-list entry are empty. An activation reference pins the
  synchronization object.
- With SLOW set, Wine's legacy fields, owner reference and owner-list entry
  are authoritative. The word blocks client CAS operations; its old owner
  bits are not an additional authority.

Under the server lock, `pw_mutex_word_freeze` sets SLOW and returns the
snapshot that must be adopted exactly once. A live owner transfers into
Wine's legacy ownership reference/list. A terminated or missing owner must
be handled as abandoned, never added to a dead thread's owner list.

Before publishing fast state, the caller checks all waiter, abandonment,
retirement, module, recursion and operation-depth conditions. It prepares
the word with `pw_mutex_word_compose`, removes the legacy owner reference
and list entry while the activation pin keeps the sync alive, then calls
`pw_mutex_word_publish`. Counts above the representation limit stay in
legacy mode and can return to fast mode after releases; they are not a new
guest error or a reason for permanent retirement.

The activation reference pins `mutex_sync`, not the named wrapper. Wine's
wrapper/name can disappear after the last handle closes while the owned
sync remains alive. Retirement must freeze/adopt ownership before dropping
the activation and wrapper references. Permanently pinning the wrapper
would change this behavior.

## Server preparation

The server hooks freeze/adopt before ordinary queue insertion, signaled
checks, ownership changes, release, query, SignalObjectAndWait, dump and
0790's `try_fast_mutex` in `server/mutex.c`. Wrapper close/destruction and
alias allocation retire the cell before dropping references or publishing
a new alias. The generic handle reference hook covers inheritance; the
explicit duplication hook also covers same-value access rewrites. Global
handles are excluded. An unconverted reader cannot be shipped alongside
active cells.

Leaving SLOW must happen after a complete top-level operation. `wake_up`
and multiwait completion can reenter mutex operations; publishing from a
nested queue callback could expose partially updated legacy state. The
server now has operation-depth and pending-publication handling. The outer
request/direct/typed operation reconciles before lock release; queue
callbacks only mark pending work. This source preparation still needs the
owner's full Wine waiter/reentrancy tests.

Normal thread exit and asynchronous termination require complete server
and client lifetime review. A unique counter token prevents a freed or
reused Wine tid from becoming an apparent live owner. Ordinary abandonment
must remain prompt for queued waiters, and server inspection must recognize
a terminated or vanished fast owner. The server code folds a currently
published fast owner before ordinary abandonment and treats a vanished or
terminated token as abandoned on slow entry. This has not been tested with real asynchronous termination or
signals and does not yet prove that protocol.

The internal activation policy excludes named, initially inheritable,
global and previously aliased/retired objects. Cells use separate aligned
allocations, avoiding a fixed arena capacity; retired word storage is
intentionally retained for the module lifetime. Allocation/token exhaustion
falls back. Memory growth must be checked during the 600-second gate.
Normal contention, multiwaits and deep recursion can recover fast mode when
their conditions permit it.

The owner's protected-route census reported 267 mutexes, four named and
none of those hot; its latest top-five rows have no named/duplicate/open/
multiwait/signal-wait/query/abandonment/timeout activity. That supports the
policy for this run. Inherit attributes were not emitted in these rows;
full coverage of that restriction remains unproven. The owner separately
reports null security attributes at the hot creation sites from static
analysis; this is not an independent source or runtime check. The other
agent owns the requested runtime attribute capture.

## Experimental Unix client

`WINE_PS5_MUTEX_SHARED=1` or a prefix-local `pw_mutex_shared` containing
exactly `1` (optionally followed by a newline) selects this backend.
`WINE_PS5_MUTEX_SHARED=0` or a prefix-local file containing `0` with an
optional newline disables it. An explicit environment value wins. With
0880, a missing setting defaults to the shared backend; malformed values,
file read/open errors other than a missing file, or allocation failure
keep it off. Missing/incompatible server ABI exports retain ordinary Wine.
The older typed `pw_mutex_fast` switch stays independently default off.
`WINE_PS5_SERVER_DIRECT=0` disables both. The setting is read once during
connection and the switch file is closed immediately.

The original mutex-only results above used explicit selection with the
0820 modules. The combined default-on acceptance is recorded separately.
Patch 0880 also selects image-view descriptor cleanup by default, with its
independent explicit off switch; see
[the lifetime contract](IMAGE_VIEW_FD_LIFETIME.md). Subsequent runtime changes
still require matching module identities, ordinary PE semantics, HL2
timedemo/clean exit, load and 600-second gameplay comparisons. Default-on
acceptance does not establish the separate 58/50 FPS target.

The client discovers `pw_wineserver_mutex_backend` and validates version,
structure size, word size, native pointer size and both required pointers.
Missing/incompatible exports retain the existing backend selection without
allocating a shared cache or acquiring its lock. The connection retains
the server module until teardown, just as for the existing direct-call
function pointers; this API does not support unloading that module while
clients run. Readiness uses acquire/release atomics and downgrade retires
active cells before server-lock release.

The canonical handle table uses 128 pages of 64 KiB on the native 64-bit
build, allocated only on demand. Empty, permanently ineligible and positive
slots are distinct. A positive slot packs the permanent native cell pointer
with its wait-access bit; release retains Wine's access-zero behavior.
Invalid handles and allocation/token/readiness failures remain retryable
and cannot create permanent negatives. More than 64 valid event handles
can retain independent negatives without the old modulo hint collisions.

All fills and their second lookups happen under `fd_cache_mutex`, with the
existing uninterrupted-section machinery. Invalidation sits beside all
four `close_inproc_sync` calls: ordinary close, duplicate-close-source,
APC-result consumption and context-handle consumption. Cache pages and
retired words stay readable through the module lifetime.

The per-thread token occupies the old four-byte padding after `ps5_sleeps`.
SDK assertions and compiled layout constants confirm `thread_data` size
and its signal-stack/sleep-field offsets are unchanged. Both thread-id
initialization sites explicitly reset the token. A new thread obtains its
own token on the first positive metadata lookup, even if another thread
already populated that handle's process-wide slot.

A warm success performs readiness/slot/word loads and one ownership CAS.
It takes no server/cache lock, makes no server request, changes no signal
mask and reads no clock. Timeout storage is read before an acquisition
CAS; the caller's previous-count storage is written after a successful
release. These source properties still require real Wine exception and
signal validation. With this backend selected, misses go to ordinary Wine
semantics rather than adding a second typed-mutex probe. Successful shared
hits also do not refresh the legacy last-server-request watchdog timestamp.
With that diagnostic enabled, its elapsed-request age cannot establish
whether a thread was idle or executing shared hits.

## Entry cost

A Unix ntdll hit can remove request marshalling, global server locking,
signal-mask changes and clock calls, while retaining the WoW64/syscall
transition. Actual console operation cost must be measured.

After that backend passes console gates, a native hit at wowprospero's
syscall BOP can call the same narrow helper before exiting the run loop.
It requires its own register/stack/FP, signal, module and diagnostic
contracts. This avoids adding a guest-writable arena or changing PE
modules. No BOP interception exists in this experimental backend.

## Checks

`python3 tests/test_wine_shared_mutex_word.py` compiles the exact header
extracted from 0810. It checks independent sequential ownership/recursion
results across mode transfers, an ordinary queued handoff, the field
boundary, retirement, and 80,000 legal critical sections across four live
native threads. Native output storage is local, never an application
pointer.

The same command also compiles the actual newly added server helper bodies
extracted from 0810, with fixture list/refcount and legacy-operation
callbacks. It checks 4,775 ownership/refcount observations, repeated slow
adoption, nested ordinary queued handoff, count-boundary recovery, owned
close, and readiness downgrade retirement. The fixture frees retained cells
only after all test readers have gone; production does not recycle them.
Both binaries pass normally and under clang ASan/UBSan.

These checks establish primitive behavior and fixture-based server transfer.
They do not run Wine, a game, console inputs, signal/termination races, or
fault workloads, and do not prove the full integration, handle lifetime,
exception behavior, abandonment protocol, or performance target.

The initial server preparation applied all 57 ordered patches to the pinned Wine
revision. Reapplying the revised 0810 to its preceding server files produces
all eight affected source/header files byte-identically to the SDK inputs.
The server module rebuild compiles all 45 C units against one private
thread/header layout, with 283 transitive file pins. The unchanged version
object is copied by the accepted recipe.

The baseline control reproduces the accepted server module byte-for-byte.
Copying the headers uniformly first shifted 17 assertion line constants
from 121 to 126 after 0770's five declaration lines; a control-only `#line`
directive preserves the accepted diagnostic positions. Candidate headers
use their actual source positions and receive no such directive. The signed
server-only candidate was preparation evidence; it is not interchangeable
with the later complete pair accepted on the console.

`python3 tests/test_wine_shared_mutex_client.py` compiles the exact added
client bodies and native ABI header from 0820. It also compiles the actual added
server lookup/ABI entry bodies. The generalized switch body is reconstructed
from 0790 with each replacement verified against 0820's actual changes.
It reconstructs the final switch body through 0880 and checks independent
typed-off/shared-on defaults, explicit environment/prefix overrides,
malformed settings and file read/open errors. The other cache/hit checks
remain unchanged. With fixture metadata, server context and
uninterrupted-section callbacks, it checks ABI mismatch rejection, 15 cold
lookup rejection gates with unchanged outputs/errors/references, 24,000
warm operations without extra cold calls/locks, 80 exact negative slots,
transient retries, recursion/local output, wait access versus access-zero
release, a second page, readiness downgrade, temporary SLOW recovery, ordinary close/reuse and
12,000 protected sections across four live threads. Normal and clang
ASan/UBSan runs pass. These callbacks do not model the complete Wine server,
its signals or exception delivery; they do not measure the WoW64 entry or
establish a console speedup.

The complete matching SDK pair rebuild compiles every server and ntdll
unit against private, consistent headers. Its baseline arms reproduce both
accepted module hashes byte-for-byte. Applied source and transitive header
pins, layout probes and separate module hashes accompany the pair. The
server control retains the previously documented assertion-line metadata
mapping; no candidate header is rewritten. The ordinary Wine matrix and
combined default-on acceptance are recorded above. They do not validate
later source revisions or establish the broader asynchronous contracts.
