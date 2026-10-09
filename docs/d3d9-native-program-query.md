# Native program readback

The helper resolves the program helper's typed native object and calls its real
GetFunction/GetDeclaration. SIZE forwards NULL data and returns native size and
HRESULT. READ first queries required size, bounds allocation to 256 KiB (65
vertex elements), then calls the backend with the original guest API capacity.
It returns owned chunks with explicit little endian tokens or declaration fields.
No creation payload or guest pointer is used as a substitute for native output.

Program objects are immutable and the service must retain a registry pin during
each call. Repeated READ requests may reconstruct the same bounded native data;
they preserve the native returned size and HRESULT, including DXVK's successful
zero/partial shader copies and declaration input-count handling. Native scratch
always has space for the required object, including when declared input capacity
is smaller. The guest adapter owns memory validation and copying to the caller.

The actual PE64 fixture compares chunked helper replies against direct backend
calls at capacities 0, 1, 3, 17 and 8192 for VS, PS and explicit/implicit FVF
declarations. Its 5000-byte shader crosses chunk boundaries; untouched trailing
bytes are compared too. Three real DXVK processes exercise creation and teardown.
This proves the helper and codec, not the full PE32 COM/session or console game.
