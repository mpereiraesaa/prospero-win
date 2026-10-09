# Native pipeline transport proof

`tests/lab/d3d9_pipeline_native.py` prepares one paired production proxy/service
build with `--draws --pipeline` and requires the negotiated mask 522239 on both
compiler commands and the build receipt. It uses the existing Transform/draw
client and implicit-surface lifetime fixtures unchanged. Only the service's
PS5 window association adapter is replaced by the existing ordinary-host adapter.

For three deterministic seeds, the same client runs directly against native
DXVK, through the proxy with async disabled, and through the proxy with async
enabled. Matrix bits, HRESULT transcripts and pixel readbacks must agree. The
controlled post-Reset UP and vertex/index-buffer draws must produce red pixels.
The separate lifetime client checks borrowed RT/depth identities, early and late
Reset failures, live public resources blocking Reset, recovery, mirror-failure
retirement and final zero references in all three modes.

The runner requires pipeline publication/acknowledgment accounting to balance,
zero pipeline publications, acknowledgments and pending peak with async disabled,
and a pending peak greater than one and no greater than eight with async enabled. These are actual observations, not inferred from a build
flag. `--allow-single-outstanding` is diagnostic only and records the relaxed
requirement explicitly; such a run cannot establish concurrent outstanding work.
A fast backend may not produce a greater-than-one peak for this workload. The
separate controlled pipeline fixture remains the deterministic ordering/fault
oracle.

Use a new isolated prefix and persistent output directory:

```sh
python3 tests/lab/d3d9_pipeline_native.py \
  --production-root /path/to/frozen/production-source \
  --recovery /path/to/persistent/recovery \
  --prefix /path/to/new-isolated-prefix \
  --output /path/to/new-proof-output
```

An optional `--pair` reuses a production build only after verifying its recorded
source hashes and both binary hashes. The host adapter is rebuilt from the same
verified recipe. Receipts freeze local source/header inputs, commit/dirty state,
commands, logs, binary hashes and observed counters. The retained host proof below covers the frozen pipeline source. It does not
establish console compatibility or game performance.

## Mixed pairs

The separate `d3d9_pipeline_mixed.py` runner takes `--old-pair` (260095) and
`--new-pair` (522239), plus recovery, new prefix and output paths. It verifies
binary hashes against both retained build receipts, then calls the real proxy
factory entry point in both cross-pair combinations. Both must return NULL and
report service startup rejection before object publication. The changed ring
size rejects these pairs before HELLO; this is not a claim that the HELLO
feature-comparison branch executed. No host window adapter is needed.

## Retained host result

Frozen runner `aecb1feb` tested production source `92c35e5d` with the actual
pinned native backend. The primary receipt contains 194 source/header inputs,
eight binaries and 19 successful commands. Three seeded native/OFF/ON transcript
comparisons and controlled red readbacks passed, as did the separate lifetime
clients in all modes. Controlled command RPCs changed from 56 to 12 with 44
queued commands in each seed. Published/acknowledged batches were 498/498,
522/522 and 543/543; the first seed observed pending peak two, and the others
observed one. Disabled runs had zero pipeline counters. This establishes actual
concurrent outstanding work for one observed run, not an eight-deep GPU workload.

Primary receipt SHA256:
`ebd25fd5e5ea4e50a0249129f0410c2275f1f2f423548ef978b299964fde7ae8`.

The separate four-command mixed-pair receipt passed both old/new directions.
Each service reported opcode zero, startup rejection, and the guest received no
factory. Receipt SHA256:
`f18f34f58d4dbdc0ca9a13c2cb4adfea061fab8b9ceac400814a074ab4dda081`.
All retained source, binary and log hashes were checked after execution.
