# Ordered batch runtime checks

`tests/lab/d3d9_batch_transport.py` builds one production pair and runs the existing D3DX compiled-effect smoke with `PW_D3D9_ASYNC=0` and `1`. It checks real stateblock restoration, getter ordering, drawing, readback pixels, Reset, and matching Present telemetry. The native window adapter is test-only; shipping service bytes remain separate.

`tests/lab/d3d9_batch_lifetime.py` runs the native control and both admission modes through the existing implicit-surface lifetime and Reset failure/recovery matrix. No console result is implied by these host checks.
