# Shared mutex backend

Status: experimental, default-off Unix client/server backend implemented,
build-tested and measured on the console.
Patch 0810 supplies server authority/lifetime hooks; 0820 adds the native ABI,
client cache and default-off switch. Native fixtures and SDK pair builds
pass. The original matching pair has completed ordinary 480-second and
600-second console runs. Real Wine semantic checks and asynchronous
thread/signal behavior remain pending; native checks and FPS observations
do not establish those contracts or justify enabling the switch by default.

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
target. The ordinary 32-bit Wine semantic comparison, exact settings/module
receipts and matched load-time gate remain pending. Later cold-admission,
retained-cell-cap and dump changes were not part of these measured runs.
Keep their source review and runtime comparisons separate.

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
exactly `1` (optionally followed by a newline) selects this backend. An
explicit environment value wins. The switch defaults off and is separate
from the older typed `pw_mutex_fast` switch. `WINE_PS5_SERVER_DIRECT=0`
disables both. The switch file is read and closed once during connection.

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
server-only candidate was preparation evidence; the complete pair below
still requires the owner's source/runtime checks before deployment.

`python3 tests/test_wine_shared_mutex_client.py` compiles the exact added
client bodies and native ABI header from 0820. It also compiles the actual added
server lookup/ABI entry bodies. The generalized switch body is reconstructed
from 0790 with each replacement verified against 0820's actual changes.
It checks strict default-off file/environment selection and independence
of the typed and shared switches. With fixture metadata, server context and
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
mapping; no candidate header is rewritten. Full source review and the
owner's real Wine matrix remain required before runtime acceptance or a
default-on decision.
