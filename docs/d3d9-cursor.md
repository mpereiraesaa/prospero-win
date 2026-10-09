# Real cursor frontend and native adapter

The module installs exact device slots10 SetCursorProperties,11 SetCursorPosition and12 ShowCursor. Surface bindings resolve through the same-device typed proxy registry; the native callback atomically acquires one owned surface interface and releases it after the real backend call. Null surfaces reach the native method for its actual validation. No native pointer crosses transport.

Position coordinates preserve signed bits and all flags. ShowCursor preserves actual input/output BOOL bits. Void/value method failures invoke nonblocking sticky cancellation; malformed successful replies do likewise. A strong device reference spans resolution, callback, validation and failure reporting, including reentrant release of the caller's last reference. HRESULT methods return the native result directly.

## Validation

`/tmp/prospero-d3d9-cursor-r2/receipt.json` passed controlled actual PE32 and PE64 typed frontend/native ABI checks, including signed minima, noncanonical BOOL bits, owned surface release, void dispatch and failure/lifetime cases. Three actual PE32-bootstrap/native-PE64 DXVK processes compare direct/helper SetCursorProperties HRESULT for a real32x32 surface and null, execute native cursor position and compare ShowCursor results. System cursor position is restored during fixture cleanup. The source-frozen runner requires all native processes to close cleanly. Production session and PS5 console coverage remain separate.
