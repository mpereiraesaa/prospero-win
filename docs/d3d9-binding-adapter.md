# Typed binding admission callback

With `PW_D3D9_ENABLE_BINDING_TICKETS`, six device binding wrappers can delegate
whole admission to an installed typed callback. They provide the original local
COM address, expected kind, exact object-word offset and copied scalar arguments.
They do not resolve an object before acquiring the session admission gate. The
callback must validate device/session health, resolve and pin a private ticket
under the family cache lock, then publish or synchronously dispatch without an
admission gap. Every returned HRESULT is forwarded unchanged.

No callback installed, or a build without the feature, retains existing typed
resolution and synchronous command routing. The existing method-ops ABI is
unchanged. Installation occurs once before vtable publication. This module alone
does not negotiate or activate queue eligibility.

The controlled fixture exercises all six families, null objects, all scalar
fields, four exact HRESULT outcomes, no preliminary resolver/command call, and
callback-free fallback in PE32/64 enabled/disabled configurations. Existing typed
method regression coverage is retained. Production session validation and ticket
acknowledgement are separate integration requirements.
