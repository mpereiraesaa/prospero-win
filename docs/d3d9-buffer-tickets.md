# Private buffer queue tickets (inactive)

Vertex and index buffers use the private ticket contract from program proxies,
behind the same disabled `PW_D3D9_ENABLE_BINDING_TICKETS` build feature. Typed
resolution checks parent, kind, canonical identity and positive public references
under the cache lock. Public Release0 remains a remote barrier even while private
tickets retain the shell. A finishing sentinel protects completion during that
barrier. Deferred and failed-defer cleanup preserve the parent and staging data;
local staging disposal follows remote retirement or terminal session cancellation.

Admission must acquire and publish while holding its session gate, detach tickets
there on acknowledgement/cancellation, then drop them after unlocking. The helper
alone does not enable asynchronous bindings or replace immediate HRESULT checks.

The controlled test covers both kinds, invalid typed resolution, duplicate
private pins, public-zero barrier, completion inside release, deferred cleanup,
failed defer, terminal failure and final parent balance. Enabled and disabled
PE32/64 builds are separate controls. Live low32 staging retention is checked
in PE32; PE64 explicitly retains the existing allocation rejection. No production queue/backend proof is claimed.
