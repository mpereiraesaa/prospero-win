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
zero pipeline publications with async disabled, and a pending peak greater than
one with async enabled. These are actual observations, not inferred from a build
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
commands, logs, binary hashes and observed counters. This runner is prepared
for the pipeline source; runtime acceptance remains pending execution. It does
not establish console compatibility or game performance.
