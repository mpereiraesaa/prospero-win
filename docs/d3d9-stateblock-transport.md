# Actual stateblock transport regression

The PE32 fixture uses production session APIs, shared-memory transport, service registry, command/getter dispatch, stateblock codec and real native DXVK helpers. A test-only native device adapter creates an ordinary native window because the host backend cannot perform PS5 window association. It does not replace the session or stateblock protocol. This fixture does not prove PS5 association or production device COM proxy installation.

Each successful cycle opens a session and device, begins a stateblock, sets a null vertex shader and a render state, ends and captures the stateblock, changes state, applies the block and reads back the restored native render state. It releases block, device and factory and cleanly closes the session. Three complete cycles are required.

## Matched negative and positive evidence

Baseline frozen source `ea153ed` fails at the first BeginStateBlock with HRESULT80004005 through the actual transport, matching the console symptom. Receipt: `/tmp/prospero-d3d9-stateblock-transport-negative-r2/receipt.json`, status `expected-rejection`. The service passes its8192-byte scratch capacity to a stateblock encoder requiring exact16 bytes, so encoding fails after native Begin succeeds. An initial attempt failed prefix setup before this test; r2 created the independent prefix and reproduced the intended failure.

The positive run changes only production fix `d436a57`: its service reply wrapper bounds available capacity and invokes the fixed-size codec with exact16. The adapter and fixture are byte-identical across baseline and corrected runs. `/tmp/prospero-d3d9-stateblock-transport-positive-r1/receipt.json` passed all three clean cycles. Frozen-source comparison found only session.c and service_stateblock.c changed; the test adapter and fixture were identical.

Run `tests/lab/d3d9_stateblock_transport.py` with `--wine-build`, `--prefix`, `--backend64` and `--output`. Add `--expect-rejection` only for the frozen unfixed baseline. The runner freezes source hashes, captures both binaries and real backend hashes, and requires all three clean cycles for success.

## Evidence retention after workstation reboot

The October 9 workstation reboot removed the `/tmp` artifacts named above, including the original receipts, binaries and logs. Those paths describe historical reviewed runs; they are no longer available for independent hash verification. The fixture sources and review history survive in Git. The historical receipts were not reconstructed. A separate fresh execution is recorded below.


## Fresh retained host proof — October 9, 2026

A new execution passed all three cycles with `status=0 restored=1` and clean
client exit 0. It used the unchanged fixture branch at
`52f9186940373dc8f7aafa9439b357850cede65a`, including the capacity-aware reply
helper, the recovered host runtime with native-domain patches 0902–0905, and the
exact DXVK 2.6.2 backend. The prefix was an isolated copy of the initialized host
prefix. No console was accessed.

Persistent evidence is under the sibling artifact directory:
`../prospero-win-artifacts/bridge-recovery-20261009/transport-recovery/stateblock-r2/`.

- `verified-receipt.json`: checks 134 local source/fixture hashes, including all
  62 local headers, plus PE client/service/backend hashes and six recovered host
  runtime output hashes. Its SHA256 is
  `aff26a9f7c716df5aec9ca7b925af8e8f52075dffd34f7bfbbe35330d0c8c26d`.
- `execution.json`: records the exact commands, environment, frozen source
  revision, child PIDs, terminal exit codes and source hashes before execution.
- `fixture/receipt.json`: retains the original runner's successful result and
  binary hashes. The execution wrapper supplies the additional header coverage.
- `command-*.stdout` and `command-*.stderr`: retain direct child output without
  inherited-pipe waits. `fixture/transport.log` contains all three restored-state
  rows.

| Artifact | SHA256 |
| --- | --- |
| PE32 client | `d92be104674c1a35ef8f55ec73b76b1d68dde91583af5d88d6098513337276af` |
| PE64 service | `8e45a1da01c176f7da80aec5f19946bcd119dcdddb1ba6acebbdec4cf53167ed` |
| DXVK backend | `1d626ff743d93f78e1369c1f28daf5bb3b7c0832eef2876a6f352ec48f575710` |

The preceding `stateblock-r1` attempt was deliberately interrupted during
compilation before any Wine/client launch; its receipt remains marked
`interrupted-before-runtime`. It contributes no runtime result.

This fresh proof exercises the frozen production session/stateblock transport
through the ordinary native test-window adapter. It does not prove PS5 window
association, production COM identity installation, the latest combined runtime
package, or console/game rendering. The historical negative was not rerun.
