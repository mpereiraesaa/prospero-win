# Shared mutex backend

Status: preparatory atomic primitives and bounded native checks. Patch 0810
adds a header only. It activates no mutex, exports no callable fast path,
changes no default, and establishes no console performance result. The Unix
client/server backend below still requires implementation and validation.

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

## Integration still required

All paths into legacy state must first freeze/adopt the word. This includes
ordinary wait queue insertion, release, query, SignalObjectAndWait, dump,
destruction, abandonment, and 0790's `try_fast_mutex` in `server/mutex.c`.
An unconverted reader cannot be shipped alongside active cells.

Leaving SLOW must happen after a complete top-level operation. `wake_up`
and multiwait completion can reenter mutex operations; publishing from a
nested queue callback could expose partially updated legacy state. The
integration needs explicit operation-depth and pending-publication
handling, with immediate reconciliation at safe server request boundaries
and before server-lock release. Relying only on a periodic tick would make
ordinary contention expensive for too long.

Normal thread exit and asynchronous termination require complete server
and client lifetime review. A unique counter token prevents a freed or
reused Wine tid from becoming an apparent live owner. Ordinary abandonment
must remain prompt for queued waiters, and server inspection must recognize
a terminated or vanished fast owner. The primitives do not implement or
prove that protocol.

The first activation policy can exclude named, inheritable or duplicated
objects only after runtime evidence confirms that this covers the measured
hot workload. Unsupported objects retain ordinary Wine semantics. Normal
contention, multiwaits and deep recursion must recover fast mode when their
conditions permit it. The other agent owns the runtime eligibility census.

The client handle table needs exact negative entries for ineligible objects,
fill under `fd_cache_mutex` with a second lookup, and invalidation beside
all four `close_inproc_sync` sites. Module discovery needs an ABI/version
handshake and an unlocked readiness check. Missing or incompatible exports
must fall back before cache fill or server locking.

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

These checks establish primitive behavior and modeled reference transfer.
They do not run Wine, a game, console inputs, signal/termination races, or
fault workloads, and do not prove the full integration, handle lifetime,
exception behavior, abandonment protocol, or performance target.
