# Actual query session transport proof

The fixture runs the PE32 Query9 COM frontend through production copied session
transport, the typed service registry and the native Query9 helper against DXVK.
Only its native window adapter and minimal local device-parent shell are test
scaffolding; they bypass PS5 guest-window registration without replacing query
operations, session framing or native query objects.

Three cycles cover genuine CreateQuery support probes, OCCLUSION BEGIN returning
S_FALSE with untouched seeded bytes, END/FLUSH/NULL-size polling, EVENT cached
partial writes, normal COM retirement, and STOP cleanup of a deliberately retained
remote query after its parent guest reference is released. This establishes host
transport behavior, not PS5 driver association or console acceptance.

## Proof artifact recovery

The three-cycle host result was reviewed before the host reboot on 2026-10-09.
Its temporary logs and binaries did not survive. Treat that result as historical
review evidence; a new retained runtime receipt is pending reconstruction of the
host native-domain runtime. Future receipts belong in persistent artifact storage.
