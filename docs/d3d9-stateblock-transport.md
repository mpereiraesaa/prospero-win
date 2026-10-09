# Actual stateblock transport regression

The PE32 fixture uses production session APIs, shared-memory transport, service registry, command/getter dispatch, stateblock codec and real native DXVK helpers. A test-only native device adapter creates an ordinary native window because the host backend cannot perform PS5 window association. It does not replace the session or stateblock protocol. This fixture does not prove PS5 association or production device COM proxy installation.

Each successful cycle opens a session and device, begins a stateblock, sets a null vertex shader and a render state, ends and captures the stateblock, changes state, applies the block and reads back the restored native render state. It releases block, device and factory and cleanly closes the session. Three complete cycles are required.

## Matched negative and positive evidence

Baseline frozen source `ea153ed` fails at the first BeginStateBlock with HRESULT80004005 through the actual transport, matching the console symptom. Receipt: `/tmp/prospero-d3d9-stateblock-transport-negative-r2/receipt.json`, status `expected-rejection`. The service passes its8192-byte scratch capacity to a stateblock encoder requiring exact16 bytes, so encoding fails after native Begin succeeds. An initial attempt failed prefix setup before this test; r2 created the independent prefix and reproduced the intended failure.

The positive run changes only production fix `d436a57`: its service reply wrapper bounds available capacity and invokes the fixed-size codec with exact16. The adapter and fixture are byte-identical across baseline and corrected runs. `/tmp/prospero-d3d9-stateblock-transport-positive-r1/receipt.json` passed all three clean cycles. Frozen-source comparison found only session.c and service_stateblock.c changed; the test adapter and fixture were identical.

Run `tests/lab/d3d9_stateblock_transport.py` with `--wine-build`, `--prefix`, `--backend64` and `--output`. Add `--expect-rejection` only for the frozen unfixed baseline. The runner freezes source hashes, captures both binaries and real backend hashes, and requires all three clean cycles for success.

## Evidence retention after workstation reboot

The October 9 workstation reboot removed the `/tmp` artifacts named above, including the original receipts, binaries and logs. Those paths describe historical reviewed runs; they are no longer available for independent hash verification. The fixture sources and review history survive in Git. No result or receipt has been reconstructed as a new execution. Future runs must use a persistent output directory and record fresh artifact hashes.
