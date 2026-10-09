# Typed D3D9 device command and getter methods

`pw_d3d9_device_methods_install` installs exactly 40 command and 28 getter slots
in the owning proxy's device vtable. Other slots retain their owner's values.
The owner calls install once, under its vtable initialization synchronization,
before publishing the vtable. The callback table is copied and then immutable.

Command callbacks receive bounded copied payloads. Required pointers are checked,
even for zero-count shader constants; Clear permits a null rectangle list at
count zero. BOOL inputs are canonicalized to the portable codec's 0/1 format.
Signed integers and float bits are preserved. Pointer-free native structure sizes
are compile-time checked for actual PE32 and PE64 builds.

Object setters resolve same-device local COM proxies to typed ID/generation pairs.
Null bindings remain null. Failed resolution stops dispatch. The caller owns its
input COM reference throughout the synchronous call; the session callback must
pin the device/session and atomically validate remote objects before native use.

Getter callbacks return the backend HRESULT and a validated reply. Output is
copied only after success and exact method, byte-count, and HRESULT checks.
Malformed successful replies mark the device sticky failure via `ops.fail` and
return E_FAIL without changing output. The four value-return methods have no
HRESULT channel; failed callbacks return zero and the callback records device
failure. Successful BOOL and floating values preserve their exact bits.

## Proof

`tests/lab/d3d9_device_methods.py` compiles the same controlled callback fixture
against Wine headers on the host, repeats with ASan/UBSan, and builds/runs real
PE32 and PE64 executables. All 68 typed slots are exercised with payload and
selector checks. Negative coverage includes required pointers, oversized arrays,
maximum and zero arrays for all six setter/getter pairs, object resolution errors, malformed replies, and unchanged output on failure.

Receipt: `/tmp/prospero-d3d9-device-methods-r3/receipt.json`.
This proof covers typed COM entry signatures and codec mapping. Production session
wiring, native backend execution, and console performance are separate checks.
