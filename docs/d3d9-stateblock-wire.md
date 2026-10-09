# Bounded stateblock transactions

Outer opcode 26 uses device targets for CreateStateBlock59, BeginStateBlock60,
and EndStateBlock61, and stateblock targets for Capture4 and Apply5. Kind10 is
reserved for stateblocks. Target IDs/generations stay in the outer envelope;
GetDevice uses a strong local parent-device identity.

Request: 16 little-endian bytes containing version1, method, stateblock type,
and a zero reserved word. Only Create carries a type. Unknown type values remain
unchanged for native API validation. Unknown methods and nonzero unused fields
are rejected before dispatch.

Reply: 16 bytes containing version1, exact HRESULT, object ID and generation.
Successful Create/End replies require both ID and generation nonzero. All other
replies require a zero object reference. Failed creation cannot publish a handle.
Decoders and encoders leave their output untouched on malformed data.

Wine's builtin D3DX effect implementation needs actual BeginStateBlock/EndStateBlock,
Capture and Apply to save and restore effect pass state. The codec does not fake
success for any of those calls. Native calls, registry ownership and typed COM
proxy methods are separate integration layers.

The focused host fixture covers all five methods, exact successful/failing HRESULTs,
creation references, reserved/version rejection, truncation and atomic failure.
Run `make build/host/test_d3d9_stateblock_wire` or compile the test and codec directly.
