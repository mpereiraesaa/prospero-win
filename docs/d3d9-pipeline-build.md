# Opt-in bounded pipeline build

`python3 tests/lab/d3d9_game_build.py --draws --pipeline --output <new-directory>`
compiles a paired PE32 proxy/native PE64 service with feature mask 522239. The
additional pipeline bit is 262144. Both endpoints require exact HELLO feature
and ring-size agreement before object creation. Pipeline builds use symmetric
65536-byte rings with unchanged 8192-byte frame scratch buffers.

Without `--pipeline`, existing feature masks and 8192-byte rings remain intact.
Even when compiled, `PW_D3D9_ASYNC=1` is required to queue and pipeline calls;
ASYNC unset/zero executes the synchronous path with the same paired binaries.
The service always preserves order and acknowledges each published batch.

The metadata fixture uses fake compiler output to check four builder option
combinations, flags, codec linkage, masks and sizes. It is not compilation or
runtime acceptance. Actual both-ABI builds and native/OFF/ON pixel, stateblock,
Reset and lifetime comparisons are required before deployment. Source receipts
and hashes belong to each immutable build; do not reuse a prior feature mask's
runtime proof as evidence for this pair.
