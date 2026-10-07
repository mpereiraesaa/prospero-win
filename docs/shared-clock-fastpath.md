# Shared clock fast path

The arithmetic core in `wine/ps5/time/pw_qpc_clock.h` describes a pointer-free,
48-byte clock anchor with the same layout for a 32-bit client and 64-bit
provider. It preserves the provider's current 10 MHz counter epoch. It does
not itself enable a clock bypass or modify Wine.

The provider must establish an invariant, synchronized TSC across every CPU
on which game threads may run. CPUID's invariant-TSC bit alone is insufficient:
measure migration, cross-core skew, drift against the same clock used by Wine's
normal counter, and behavior under load and suspend. Unsupported or unverified
systems must use the normal counter. No kernel control-register changes are
needed.

A provider constructs an anchor after this validation, using an ordered TSC
bracket around its normal performance counter. The client reads a coherent
snapshot and expires it after at most one second. Backward TSC, stale anchors,
wrong versions, unsupported frequencies, and arithmetic overflow reject the
fast answer. The reader uses only 64-bit multiplication and shifts, including
explicit overflow checks; it needs no division or 128-bit helper on its hot
path. Fixed-point rounding underestimates the reference by at most three
100 ns ticks over the supported interval. Refreshes must reconcile their
counter with the normal epoch and preserve per-thread monotonicity.

Run the portable boundary and reference tests with:

```
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined \
    tests/test_pw_qpc_clock.c -o /tmp/test_pw_qpc_clock
/tmp/test_pw_qpc_clock
```

Input fast paths need a different contract. `GetKeyState` may answer from live
thread-input and desktop shared objects only after checking both object IDs,
coherent seqlocks, and the keystate-lock/desktop-serial synchronization rule.
Mappings and their entire accessed ranges must be readable below 4 GiB for a
PE32 client. A copied key snapshot is insufficient because it becomes stale
without a syscall to refresh it.

`GetAsyncKeyState` additionally pumps driver events and can flush window
surfaces before reading state. It also consumes the last-pressed bit through
its normal server request. Those observable effects require a slow path until
an independently verified event-pump eligibility rule is available. Reading
the down bit directly must not bypass them.
