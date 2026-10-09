# Actual copied UP draw transport

The fixture installs the real typed PE32 UP frontend on a minimal test device shell whose callbacks invoke production session APIs. It retains shipping session, codecs, registry, upload lifecycle, native UP dispatch and texture readback code. A test-only ordinary native window adapter replaces PS5 window association. This proves the draw transport and owned data path, not production COM identity/window installation or console rendering.

## Evidence

`/tmp/prospero-d3d9-up-transport-r4/receipt.json` passed three complete cycles on source `c575c28`. Each draws a red nonindexed triangle and a green indexed triangle with MinVertexIndex two. Pixels are copied through real native GetRenderTargetData and returned by production texture lock/read transport. A missing-declaration backend COMMIT failure is followed by a successful upload, proving the failed commit consumed its transfer. Finally an active upload is deliberately abandoned while guest device references retire; clean session close proves shutdown releases the persistent device pin. Frontend references return to their initial count with no sticky failure.

The first attempt stopped at compile-time fixture indentation warnings before runtime. Corrected r2 passed unchanged hashes across all executions. Review then zero-initialized unused indexed prefix vertices and removed an unrelated copied negative-mode option; r3 passed. Final r4 additionally freezes every local wine/ps5 header with the C and fixture sources, preserving opcode/type contracts as well as implementation bytes. The native window adapter is local to this test and cannot be linked by the production build accidentally through its explicit source lists. No production source changes are part of this regression.
