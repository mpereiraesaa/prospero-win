# Native draw batch differential fixture

This fixture is prepared for the frozen draw service from `197a6152` and
`ccf19938` (base `08ae32b7`, which also retains the separate recording outcome
fixture). Actual host proof passed from frozen fixture source `113870f3`.

`tests/lab/d3d9_draw_batch_native.py` compiles the PE64 pixel fixture plus the frozen recording outcome fixture and runs them with
the pinned Prospero DXVK backend on ordinary host Wine. It links the actual
service batch, prepared binding leases, native command dispatcher, registry,
stateblock service and codecs. Its test-only adapter maps a local device context
to the actual COM device and resolves typed registry entries. It does not test
client admission, private proxy tickets, IPC, PS5 association or console speed.

Each of three fresh device cycles compares synchronous service command dispatch
with batch service dispatch. Both paths perform the same 50 commands, including
all six non-null binding families. Vertex and pixel shader objects are bound and
then unbound before fixed-function draws; this does not claim programmable
shader rendering coverage. Both DrawPrimitive and DrawIndexedPrimitive produce
red pixels over a black clear. Full RGB readback hashes must match between paths.
The test also verifies an outside-triangle black pixel.

An active stateblock records a null declaration while a draw uses the still-live
declaration; pinned DXVK Apply skips a captured null declaration, so End and
Apply must retain the same canonical live declaration (the returned COM reference
is released). This follows `d3d9_stateblock.cpp` lines62–70.
Each path resets the actual backend, rebinds retained managed resources and
repeats pixel checks. The direct path performs 50 command service calls and the
batched path performs 10 batch calls. These are harness call counts, not measured
IPC crossings or a performance claim.

Every command boundary checks registry queue pins returned to zero. Stateblocks
are destroyed through the actual service helper, all six resources are unbound
and retired, every registry slot is free, and final device/factory Release must
return zero. The separate controlled shipping tests cover stale IDs, failure
prefixes and early/late Reset behavior; this fixture supplies real rendering and
successful Reset parity.

Run only while holding the coordinated host GPU/build slot. Use an isolated
initialized prefix and a new persistent output directory. The runner saves
terminal failures/timeouts, compiler/runtime command logs, source and all local
header hashes, backend hash and executable hash. On timeout inspect descendant
processes before any retry; it does not kill unrelated Wine processes.

The existing `d3d9_service_stateblock.c` control also runs with DRAW_BATCH.
It proves actual native End succeeds while registry publication fails with
E_OUTOFMEMORY, and recording returns to LIVE despite that outer failure.
Failed Begin/End preserve state; unexpected S_FALSE marks UNKNOWN. This source
is retained unchanged from dispatch commit `08ae32b7`.

## Retained proof

`../prospero-win-artifacts/bridge-recovery-20261009/draw-batch-native-r2/receipt.json`
records PASS: four commands exited zero, 106 source/header hashes and two PE64
executable hashes were rechecked after execution. Receipt SHA256:
`3cd5b685b5b2e1f9cc5170b6bf7494a412b36ab590e950ede95cad0fc7c15a40`.

All three cycles passed both nonzero draw types, RGB equality, six binding
families, four recording/Apply checks and two actual Reset calls per cycle.
Device and final factory Release returned zero. The unchanged recording fixture
also emitted three passing rows, including native End success with publication
E_OUTOFMEMORY. Tests ran sequentially in a private ordinary-host prefix under an
8 GiB process scope. These results do not enable client draw admission.

The retained `draw-batch-native-r1` receipt is a compile failure: the test-only
helper name `bind` collided with Winsock. Commit `113870f3` renamed that helper;
no backend or production implementation changed. Earlier draft expectation for
captured-null declaration Apply was corrected before any runtime run after
checking pinned DXVK source; the actual proof verifies retained live identity.

Publication keeps the fixture C and runner identical to the executed source.
The normalized dependency tree contains later constants/Transform policy and proxy/header
updates: 98 of the 106 captured inputs match it; the eight differences are listed
in retained `draw-batch-native-r2/publication-map.json`. The receipt proves the
frozen source named above, not a fresh run of those later dependency changes.
