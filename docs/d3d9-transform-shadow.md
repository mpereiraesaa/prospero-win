# Transform shadow for local GetTransform

GTA San Andreas calls `GetTransform` about 300 times per street frame (37047
calls in one 120-present interval of the r7b control run). Each one is a
synchronous round trip, and with batching enabled each one is also a barrier
that flushes the queued setters. This portable foundation models the device
transforms in the proxy so those calls can be answered locally. It does not
wire anything into the proxy or session yet.

## What the backend does

The contract is the pinned DXVK `5fde742bd8fc0c3e9f062caa91ef0e27786df1b4`.

- `SetTransform` copies the matrix; a NULL matrix stores identity. While a
  state block is being recorded it only records, and the live value stays.
- `MultiplyTransform` changes the live value even while recording.
- Only states 2, 3, 16 to 23 and 256 to 511 have a bounded backend slot. Other
  numbers index outside the array, so the shadow never answers them.
- `Capture` and `Apply` return `D3DERR_INVALIDCALL` while recording, before
  doing anything. `CreateStateBlock` is refused while recording too.
- A `D3DSBT_ALL` block captures every transform. Pixel and vertex blocks
  capture none. A recorded block captures exactly the transforms set while it
  was recorded.
- `ResetState` changes neither the transforms nor the recording state.

## Rules

Every slot starts unknown, even on a fresh device; no default is assumed. A
slot becomes known only from an `S_OK` `SetTransform` outside recording or an
`S_OK` `GetTransform` reply. `MultiplyTransform` makes its slot unknown
instead of repeating the float product locally. A setter for a state without a
slot makes every slot unknown.

Recording follows `BeginStateBlock` and `EndStateBlock` outcomes. When either
outcome is not `S_OK`, the recording state is unknown: the backend may have
ended recording even though End reported a failure (for example when
publishing the new block ran out of memory). While it is unknown, every live
slot is forgotten, nothing is answered or learned, and the block being
recorded is unknown. Only a successful `BeginStateBlock` or `EndStateBlock`
establishes the state again; a successful `Capture`, `Apply` or
`CreateStateBlock` does not. A refused `Capture` or `Apply` while the shadow
believed recording was off makes the state unknown too.

Each state block shell owns its evidence. Its snapshot storage is allocated
before the Create or End call, outside any lock; if that fails, the block is
simply unknown. Applying an unknown block makes every slot unknown. Applying a
known block copies its captured slots, including the ones it captured while
unknown. `Capture` refreshes the captured slots from the live shadow. A
`Capture` or `Apply` that fails with anything other than `D3DERR_INVALIDCALL`
may have run, so it forgets the affected slots. Evidence is freed with the
local shell, never on a wire release, so a duplicate remote release cannot
remove it.

Every `Reset` boundary makes the live slots unknown and keeps the recording
state and the block snapshots, as the backend does.

## Integration requirements

The caller runs every observer under the session serialization gate, after
the reply is validated and before unlock, so the shadow follows wire order.
The lookup for a `GetTransform` runs under that gate too. Observers never
allocate, lock or make a call. Callback reentry must be rejected before any
shadow access, and a failed device or session keeps its existing failure
result instead of a local answer. The feature stays behind `PW_D3D9_ASYNC=1`.

## Evidence

`tests/test_d3d9_transform_shadow.c` runs in `make test` and under the
sanitizers. It covers the slot mapping, exact Set/Get outcomes, NULL identity,
Multiply, recording, refused and failed Capture/Apply, ALL/PIXEL/VERTEX and
recorded blocks, missing snapshot storage and Reset boundaries.
