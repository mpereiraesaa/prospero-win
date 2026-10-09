# D3D9 method inventory and implementation gate

`tools/generate_d3d9_inventory.py` consumes the project's pinned Wine
`include/d3d9.h`. It rejects a different source hash, missing/duplicate methods,
unparsed declarations, changed counts and changed IUnknown slots. The generated
header covers all17 base D3D9 interfaces, including all119 device slots. Ex
interfaces are deliberately outside this version.

Each macro row has interface, slot, method, exact native return type and
parameters, and a reviewed policy. Native signatures are for local proxy
definitions and compile checks; they are never wire formats.

| Policy | Validation and ordering | Outputs and lifetime |
| --- | --- | --- |
| LOCAL_IDENTITY | Validate interface IID and proxy lifetime locally | Canonical IUnknown identity; local guest references distinct from queued service references |
| SYNC_FACTORY | Validate portable factory codec; serialize request/reply | Exact HRESULT and declared outputs; retain target until service completion |
| UNSUPPORTED | No dispatch or backend success may be inferred | A separate reviewed adapter and payload policy are required before enabling |

Policies are obligations for implementation, not a runtime enable list. The
presence of a codec alone does not enable an interface or method. Factory
slots4–14 have synchronous coverage; device and resource methods remain blocked
in this inventory until their complete adapters are reviewed. In particular,
CreateDevice, Reset and Present having portable payloads does not yet prove their
window, ownership and completion semantics.

Do not generate success stubs from this inventory. HRESULT methods need explicit
unsupported errors. Methods returning a scalar, pointer or void cannot silently
pretend to succeed; an implementation must supply the proper local semantics or
record a session failure that is exposed at a synchronous boundary. Entire
unsupported interfaces must not be advertised by QueryInterface.

Regenerate with `--header PATH --output wine/ps5/d3d9/pw_d3d9_inventory.h`;
`--check` verifies byte equality. The Python suite checks parser failures,
inventory coverage, policies, known slots and reproducibility. Compile and run
`tests/lab/d3d9_inventory.c` as PE32 and PE64: every native vtable offset, complete
vtable size and function pointer signature is checked, including calling
convention. No backend execution is claimed by these layout checks.
