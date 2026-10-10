# Native WoW64 signal provider

Patch 0883 installs PS5 signal entries that recover the host FS base from
an aligned alternate signal stack before entering Wine C handlers. It uses
the measured ordinary-title libkernel identity, segment layout and import
addresses to select raw signal registration. Unknown identities retain the
imported signal path and do not advertise native execution readiness.

The optional `__wine_prospero_native_wow64_caps(1)` export reports zero until
signal registration and the standard x87/SSE/AVX layout checks pass. The
native backend requires all three capability bits. Existing translated-code
SIGSEGV hooks remain available for wowprospero.

Run `python3 tests/test_native_wow64_provider.py` for the host identity guard
fixture. It accepts the supported module metadata and rejects malformed,
unknown and ELF-loader identities with zero output pointers. This checks
metadata only; it does not prove kernel signal delivery or guest execution.

The full patched Wine pipeline builds all PRXs without unresolved imports.
On a physical console, its freshly built ntdll passed simultaneous native
Win32 worker TLS/TEB/thread-ID checks, native Minesweeper with scripted input,
access-violation/UD2 resumption with x87/XMM0/YMM0, and a DBT Minesweeper
fallback control using that same provider. Each run exited normally with
capabilities 7 and restored and verified the original console files.

An earlier private provider also passed native and DBT Space Cadet gameplay
and audio. Those results do not establish Space Cadet correctness for the
fresh full-build provider. HL2, GTA SA, GTA IV, DXVK/d3dx9 performance and DBT
retirement remain open. The backend stays opt-in through the Wow64 registry
selection; wowprospero remains available as the fallback.
