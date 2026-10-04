# Shared mutex backend

Status: server preparation and bounded native checks. Patch 0810 adds the
atomic header and server authority, queue and lifetime hooks. The internal
cold preparation function has no runtime caller; no client can activate a
shared cell yet. No new module export or default is enabled, and no console
performance result exists for this patch. Unix client/ABI/cache integration
and full Wine runtime validation remain required.

The performance target remains the full fixed GTA IV route at 1920×1080,
60 Hz, profiling off, average at least 58 FPS and minimum at least 50 FPS,
with the existing HL2 timedemo/clean-exit, load and 1,800-second stability
gates. A helper test does not replace these gates.

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
published fast owner before ordinary abandonment and treats a vanished/terminated token as abandoned on slow
entry. This has not been tested with real asynchronous termination or
signals and does not yet prove that protocol.

The internal activation policy excludes named, initially inheritable,
global and previously aliased/retired objects. Cells use separate aligned
allocations, avoiding a fixed arena capacity; retired word storage is
intentionally retained for the module lifetime. Allocation/token exhaustion
falls back. Memory growth must be checked during the 1,800-second gate.
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

## Client integration still required

The client handle table needs exact negative entries for ineligible objects,
fill under `fd_cache_mutex` with a second lookup, and invalidation beside
all four `close_inproc_sync` sites. Module discovery needs an ABI/version
handshake and an unlocked readiness check. Missing or incompatible exports
must fall back before cache fill or server locking. Server readiness reads
and writes now use acquire/release atomics, and downgrade retires every
active cell before unlocking. This is preparatory server code; client
readiness/module lifetime checks remain absent.

## Entry cost

A Unix ntdll hit can remove request marshalling, global server locking,
signal-mask changes and clock calls, while retaining the WoW64/syscall
transition. Actual console operation cost must be measured.

After that backend passes console gates, a native hit at wowprospero's
syscall BOP can call the same narrow helper before exiting the run loop.
It requires its own register/stack/FP, signal, module and diagnostic
contracts. This avoids adding a guest-writable arena or changing PE
modules. No BOP interception exists in this preparatory patch.

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

The source preparation applied all 57 ordered patches to the pinned Wine
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
candidate is build evidence only and must not be deployed before client
integration and the owner's runtime checks.
