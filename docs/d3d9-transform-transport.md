# Combined Transform and draw transport proof

The host fixture drives identical seeded workloads through direct native DXVK,
the production proxy with asynchronous mode off, and the production proxy with
asynchronous mode on. It compares complete transcripts of Transform matrix
results, operation HRESULTs, and pixel readbacks. The production pair enables
capability mask 260095 with API observation disabled.

The workload includes Set/Get/MultiplyTransform, NULL identity, state block
recording, Create/Capture/Apply, failed and successful Reset, Present, sampler
and render state, constants, and vertex/index-buffer draws. A deterministic
stage after Reset renders both user-pointer and buffer-backed triangles and
requires red readback in every mode. An empty successful Begin/End pair first
reestablishes known LIVE recording state, so both DrawPrimitive and
DrawIndexedPrimitive are eligible for the measured queued interval.

## Retained result

The frozen fixture source is commit 3d1dfa03. The final-260095-r9 receipt records
13 commands with zero exits, 190 source inputs, and three seeds whose native,
proxy-off, and proxy-on transcripts match exactly. Every controlled pixel check
passes. The measured interval reduces command RPCs from 56 to 12, with 44
accepted batch commands: 42 setters and two draws.

Receipt SHA256:
`93ba36426cd8ef2b9ee7bb5877d8be78970f5568222cffa8681a85e75be2e4d1`.

Earlier randomized black readbacks were also reproduced by direct native DXVK;
they remain in the parity transcripts. The separate deterministic red-image
stage establishes successful rendering rather than treating equal failed
output as rendering success. Earlier r8 established parity but conservatively
kept its two controlled draws synchronous after failed-End uncertainty; r9
adds the successful recording transition and proves their batch membership.

## Running and scope

Run `tests/lab/d3d9_transform_client.py --help` for the retained runtime,
backend, isolated prefix, output, and pair-reuse arguments. The fixture compiles
actual production client/service code. Only PS5 window association is replaced
by the existing ordinary-host device adapter plus authoritative recording
tracking. It uses the same native recording outcome helper as production.

The adapter tracking table is bounded for this process-isolated fixture; it is
not production lifetime storage. Receipt source and artifact hashes preserve
the tested inputs. Pair-reuse mode requires the caller to independently verify the retained
pair source and artifact hashes against the intended shipping source. The proof is host graphics compatibility and transport evidence, not a
console result, game frame-rate claim, or general support for arbitrary draws.
