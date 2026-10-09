# Native service session ownership token

The [native service bootstrap](wine-native-service-domain.md) assigns each new root
service thread a positive process-local 64-bit token. Native child threads
inherit their parent's token. Tokens increase monotonically, are never reused
within the process, and exhaustion fails thread creation. This is lifecycle
and ownership validation, not a security boundary against arbitrary code
already executing inside the process.

## Private native thread query

Native `NtQueryInformationThread` class `0x50570002` accepts only the current
thread pseudo handle. The input/output record is exactly 16 bytes:

| Offset | Field |
| --- | --- |
| 0 | uint32 version, must be 1 |
| 4 | uint32 size, must be 16 |
| 8 | uint64 token, output |

The native Unix entry returns the same record and length on success. A wrong
length returns STATUS_INFO_LENGTH_MISMATCH; a wrong version/size returns
STATUS_INVALID_PARAMETER. Other thread handles and threads outside the native
bootstrap domain return STATUS_NOT_SUPPORTED with token zero. The ordinary
WoW64 query thunk rejects the class with STATUS_INVALID_INFO_CLASS and does not
forward the guest buffer.

The only consumer of this token, a window association in the PS5 user driver,
was removed together with the experimental D3D9 backend, so nothing in the
runtime reads it today. A future consumer must not truncate the 64-bit token
into a narrower identifier.

The v1 bootstrap still requires the service to release its resources and join
children before returning. A token is not an
allocation or a lifetime reference; storing its value does not keep a thread,
window, session or module alive. The bootstrap waits for the service entry to
return before unloading it.

## Focused fixture

The native-domain fixture checks nonzero root tokens, equal child tokens,
strict increase over ten complete bootstrap cycles, rejection of malformed
headers/lengths and duplicate real thread handles, and PE32 query rejection
before and after native service execution. Existing TLS, exception, high
allocation and DXVK factory checks remain enabled. Host/console results must
be recorded separately; no console token proof is claimed here yet.

Host proof completed with ten native and ten DXVK cycles: tokens 2 through 11
matched each child, increased across roots, and all rejection checks passed.
Receipt: `/tmp/prospero-bridge-native-session/host-proof-r1/receipt.json`.
