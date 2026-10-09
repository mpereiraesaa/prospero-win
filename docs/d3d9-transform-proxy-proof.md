# Production Transform proxy comparison

`tests/lab/d3d9_transform_proxy.py` consumes a frozen shipping pair and its separately built ordinary-window host service. It compares a native PE64 DXVK control with the actual PE32 proxy in asynchronous off/on modes, using identical backend bytes and three device lifetimes each.

The fixture records complete 64-byte matrix outputs and HRESULTs. It covers initially unknown values, repeated Set/Get on boundary selectors, negative-zero/NaN payloads, Multiply invalidation, recorded Set versus live getters, selective End/Capture/Apply, refused Capture/Apply while recording, ALL/PIXEL/VERTEX blocks, Reset and Reset during recording. It does not assume Reset ends recording. Expected outputs come from the actual backend, not locally calculated matrix multiplication.

A supplied host service replaces PS5 window association only. The runner preserves input and binary hashes and terminal failure receipts. The comparison proves output parity; a separate transport/profile check is required to establish cache-hit or round-trip reductions. It makes no console or performance acceptance claim.
