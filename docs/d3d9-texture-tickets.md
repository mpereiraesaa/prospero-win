# Private Texture2D queue tickets (inactive)

Texture2D bindings use the private ticket contract under the disabled
`PW_D3D9_ENABLE_BINDING_TICKETS` feature. Surface and implicit-owner objects are
ineligible. Ticket acquisition validates parent, canonical identity, kind,
positive public references and unfrozen/unclosed state under the cache lock.
Description cache entries remain immutable and protected by ordinary call pins.

Public Release0 still runs the remote barrier. Private tickets retain the shell,
parent, local description cache and staging until remote completion and the last
ticket drop. Completion inside Release cannot finalize its active shell. Deferred
or failed-defer cleanup retains all local state. Drop runs only after the session
admission gate is unlocked and may release the final parent reference.

Controlled PE32/64 on/off fixtures retain existing texture/surface regressions,
then cover typed rejection, duplicate tickets, cached descriptions, public-zero
barriers, completion during Release, deferred and failed-defer cleanup, terminal
failure and final reference/staging balance. Live low32 mapping is PE32; PE64
preserves its existing allocation rejection. No live binding queue is enabled by
this module alone, and this fixture does not claim backend/console acceptance.
