# Native object-returning device queries

The native helper invokes the nine typed device getters in the portable schema.
It returns the backend's HRESULT, one owned typed COM reference on non-null
success, and actual stream offset/stride where applicable. Null bindings remain
null. Failures clear all outputs and release any returned reference. Cube and
volume textures explicitly return E_NOTIMPL until their proxy types are modeled.

The result structure is service-local and must never be serialized. The caller
must transfer the owned reference to a correctly typed resource/program wrapper
or release it. Wrappers must validate GetDevice and retain their parent device;
the registry must reuse canonical IUnknown identity and manage guest references.
The outer dispatcher must additionally retain its device/window context. This
is necessary for implicit back buffers and FVF declarations and for buffers
retained by a native binding after their original guest proxy was released.

The helper revalidates request framing through the portable encoder before any
backend call. It does not fabricate registry IDs or bypass stale-object checks.
The device argument must already be a validated, pinned native device. Calls
are serialized by the native service. Returned references can be dropped with
the release helper, which clears its structure to prevent accidental reuse.

The host runner executes three real PE64 DXVK processes. It covers back-buffer
and render-target identity, exact depth-surface failure, null and bound textures,
implicit FVF declaration, vertex/pixel shaders, and vertex/index buffers queried
after their original application reference is released. Stream offset and stride
are checked. This proves backend querying and returned-reference ownership;
registry/proxy integration and console game compatibility remain separate gates.
