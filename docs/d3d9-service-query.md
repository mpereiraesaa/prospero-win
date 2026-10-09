# Service query registry adapter

Serialized CREATE dispatch targets a live device; Issue/GetData require a query
kind. Native contexts publish canonical IUnknown identities with service IDs and
generations, own their backend references and retain the registry parent through a
queued reference. Duplicate identity publication validates kind and parent, consumes
the extra native context and adds a guest reference to the canonical slot.

Temporary dispatch pins and persistent parent pins are separate. Allocation or
registry-capacity failure destroys the newly created native query and balances its
parent pin. Retirement destroys backend references before completing the persistent
parent reference. Invalid/stale targets return checked errors.

Backend GetData replies retain their copied bytes for every HRESULT, including
S_FALSE and failure. Invalid target replies preserve seeded caller bytes. The query
wire encoder accepts scratch capacity and reports the actual encoded byte count.

The fixture uses an actual DXVK device and query with the real object registry.
Three host processes verify support probes, canonical dedup using duplicated owned
native references, bounded capacity exhaustion, pending and failed data replies,
parent retirement ordering, cancellation and stale IDs. The fixture uses a local
backend-device context adapter; it does not claim production session transport or
console acceptance.
