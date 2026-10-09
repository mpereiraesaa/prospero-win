# Native owned UP draw dispatch

The helper accepts only a complete committed upload. It revalidates exact lengths, capacity and ownership, queries the real current vertex declaration and bounds the padded native allocation plus indices to 64 MiB. Declaration references are released on every path. It then calls the real DrawPrimitiveUP or DrawIndexedPrimitiveUP, retaining the complete indexed MinVertexIndex prefix and returning the exact HRESULT. The service must serialize device access and finish the upload after every dispatch outcome.

For zero primitives the helper directly calls the backend with valid local dummy pointers and unchanged scalar arguments. This preserves native declaration-first validation and binding behavior. For positive draws, copied bytes follow DXVK 2.6.2 count times stride; the backend zero pads any declaration excess. No guest pointer reaches native dispatch.

## Validation

`/tmp/prospero-d3d9-native-up-r2/receipt.json` passed host and ASan/UBSan controlled native ABI tests plus three actual PE32-bootstrap/native-PE64 DXVK processes. Each process compares zero-count HRESULT before/after FVF, draws and reads back a red nonindexed triangle and a green indexed triangle with MinVertexIndex two, checks native stream/index reset and verifies immutable input bytes. The first runner attempt stopped on a fixture indentation warning before native execution; corrected r2 passed unchanged source hashes across all runs. This is helper evidence, not production session or console coverage.
