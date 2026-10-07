# Shared key-state fast path

Patch 0901 adds an experimental PE32 fast path for `NtUserGetKeyState`.
`PW_INPUT_SHARED_FAST=1` enables low-address, read-only mappings of Wine's
existing session objects. The default retains ordinary syscalls. If a low
mapping fails, the provider retries its original mapping strategy and the
client falls back; input remains functional under address-space pressure.

A fixed, pointer-free 48-byte descriptor publishes addresses and expected IDs
for the calling thread's input object and desktop object. The provider verifies
that the complete objects fit below 4 GiB. Session views remain mapped until
process teardown. A private query through the existing two-parameter user call
fetches this descriptor; an older provider rejects it and the client retains
normal syscalls. No syscall number or dispatch-table entry changes.

The reader uses the live objects, rather than a stale copy of key state. It
validates their seqlocks and IDs, preserving the input-lock/desktop-serial rule.
An invalidated input object, changed desktop, serial mismatch, or active writer
uses the original syscall. Retries are bounded. Key indexing and sign/toggle
bits match the ordinary implementation, including indexing modulo 256.

The descriptor occupies an appended part of Wine's private per-thread client
info inside `TEB.Win32ClientInfo`. Its original 32-byte prefix is unchanged,
and static assertions bound the enlarged structure to the TEB storage. This
uses Wine's existing WOW64 thread-info accessor, without allocating TLS or
adding compiler-emulated TLS dependencies. A successful thread-desktop change
invalidates the cached descriptor. A failed change preserves it. Thread teardown
naturally releases the TEB storage.

The cache's existing status word exposes a quiet first-success marker:
`0x1` means the provider is disabled and `0x2` means this thread returned at
least one fast result. Every disabled check masks `0x1`, so the success bit
never disables the reader. Each bit uses a one-time atomic OR, preserving a
previous success when a later provider failure disables the path. Warm calls
use a normal status check and perform no repeated marker atomic or logging.
The marker is historical; a value of `0x3` explicitly means the thread once
used the fast path but now falls back. Reading it does not prove that every
later call was fast. The descriptor remains the authority for current
object IDs and synchronization.

`NtUserGetAsyncKeyState` retains its normal path for valid keys. That path pumps
driver events, may flush window surfaces, and consumes the last-pressed bit via
the server. Reading only the shared down bit would omit these effects. Invalid
keys can return zero directly, matching the original early return. Further
async-key optimization requires a verified event-pump/flush eligibility rule;
it is not claimed by this change.

Portable tests check shared-object reuse, lock/serial disagreement, odd
sequences, key wrapping, and signed bit results. Controlled page faults also
change the real input or desktop sequence after a read has begun, verifying
that the unmodified reader rejects mid-read mutations:

```
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined \
    tests/test_pw_key_shared.c -o /tmp/test_pw_key_shared
/tmp/test_pw_key_shared
```

Integration tests extract the actual staged PE wrappers and execute them with
a deterministic provider and thread-info lookup. The shared reader and wrapper
control flow are unchanged. The tests cover provider fallback, input-object
reuse, desktop-change success/failure, async-key forwarding, and eight concurrent
threads with independent caches:

```
python3 tools/test_wine_key_bridge.py --source /path/to/staged-wine \
    --wine /path/to/host-wine
```

A linked runtime and hardware input-delivery comparison remain required before
release enablement. Verify keys, toggle bits, focus changes, attached thread
input, and desktop changes alongside the measured syscall reduction. This path
remains opt-in until those checks pass.
