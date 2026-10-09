# Owned user-pointer draw uploads

The UP codec transports DrawPrimitiveUP (83) and DrawIndexedPrimitiveUP (84) through BEGIN, ordered WRITE, COMMIT and ABORT. The outer service opcode is 28. Requests have a canonical 64-byte little-endian header and at most 4096 data bytes. Replies are 24 bytes. No guest pointers cross the wire. A device owns at most one active upload, with a monotonically increasing nonzero transfer ID and a 64 MiB combined vertex/index limit. The caller owns allocation and serialization.

BEGIN validates all multiplication and addition bounds, including primitive expansion, indexed MinVertexIndex plus NumVertices and combined payload. The complete indexed prefix is preserved. For packaged DXVK 2.6.2, caller reads are exactly vertexCount times stride; declaration extent beyond stride is zero padded by the native backend, never read from guest memory. Native dispatch must also bound the declaration-derived padded native allocation plus indices. Zero primitives carry no data and still execute the real backend method to preserve declaration-first validation.

WRITE accepts only the next contiguous offset and copies into owned storage. COMMIT freezes complete storage until native dispatch returns. The service must finish the upload on both native success and native failure, so IDs cannot replay draws. ABORT retires partial storage; session teardown must release it. Successful replies carry the transfer ID; failed replies carry zero and preserve the exact HRESULT.

## Validation

`make build/host/test_d3d9_up_wire` exercises topology spans, indexed prefix, integer overflow, combined limits, canonical reserved fields, truncation, incomplete and duplicate writes/commits, copy ownership, stale IDs, abort and exhaustion. Focused ordinary and ASan/UBSan executables both passed. This layer does not execute D3D calls.
