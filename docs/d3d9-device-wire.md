# D3D9 device creation, reset and presentation payloads

The portable codec covers three explicit operations: CREATE1 (factory
CreateDevice), RESET2 (device Reset), and PRESENT3 (device Present). A target
interface is validated separately by the service; CreateDevice and Reset share
COM slot16 and therefore cannot be identified by slot alone. This codec neither
calls a backend nor implements a device proxy.

All words are little endian. Request headers are four u32 words: version1,
operation, payload size, zero reserved. Reply headers are version1, operation,
exact HRESULT bits, payload size. The service adapter verifies that the outer
channel HRESULT agrees with the reply payload.

Presentation parameters contain the 13 scalar words in
`PW_D3D9_PRESENT_FIELDS` order, then a driver-issued window triple
`{epoch,id,generation}` (64 bytes total). Boolean fields are canonical0/1;
formats, flags and sizes reach the backend unchanged. The native HWND is mapped
locally by each adapter. A triple is either entirely zero (null) or entirely
nonzero. Driver registrations have their own epoch, separate from the session.

| Operation | Request after header | Reply after header |
| --- | --- | --- |
| CREATE | adapter, device type, behavior flags; focus window triple; parameters | returned object ID/generation; parameters |
| RESET | parameters | parameters |
| PRESENT | optional-field mask, override window triple, source RECT, destination RECT, dirty count, dirty bounds RECT, reserved0, dirty RECTs | no output |

CREATE requests are104 bytes; RESET requests80. CREATE replies are88 bytes;
RESET replies80; PRESENT replies16. CREATE/RESET replies always preserve the
backend's in/out parameters, including after failure. The native adapter starts
from the decoded input, normalizes BOOLs, and maps only known local windows back
to their opaque identities. Failed CREATE replies require a zero object pair;
successful CREATE requires both ID and generation. No local pointer is returned.

PRESENT has an88-byte fixed part and16 bytes per dirty rectangle. Mask bits1/2/4
select source, destination and dirty region. Absent optional fields encode as
zero and noncanonical hidden data is rejected. Dirty regions retain their bounds
and rectangle order, including an explicitly present empty region. The adapter
validates the native RGNDATA header and buffer length and reconstructs a native
RGNDATA locally. More than256 dirty rectangles is explicitly unsupported by this
version; do not truncate a region or report success. Signed rectangle coordinate
bits survive unchanged, including negative and extreme values.

The codec validates framing, identities and bounded storage. It does not replace
backend API validation, window lifetime acquisition, sequence ordering, target
object checks or exact error propagation. Guest pointers/handles never enter
these payloads. Tests exercise all truncations, overflow counts, invalid IDs,
noncanonical optional data, small-output atomicity, failure parameters, successful
nonzero HRESULTs and both native PE parameter layouts.
