# Negotiated binding batch service

`PW_D3D9_BINDING_FEATURE` (32768) is additional to scalar batching. The service
must enable it after both peers negotiate support, before the first command
sequence. Builds without `PW_D3D9_ENABLE_BINDING_TICKETS`, or sessions without the
negotiated setter, keep the original scalar policy. This helper does not advertise
the feature or enable a shipping builder by itself.

With binding support, whole-batch canonical eligibility is validated before the
first native call. The target device generation is pinned, the immutable owned
snapshot is passed to prepared leases, and every valid referenced generation is
pinned before native acquire callbacks. Commands execute in order with that
record's typed native lease. Any native/type failure stops at its exact position;
unexecuted suffix leases are released before the device pin. Unexpected positive
acquire results are protocol failures, and sticky failure forbids further work.

The controlled fixture uses real service, lease helper, registry, codecs and native
command dispatcher with mock native COM. It checks disabled negotiation, malformed
suffix, caller input mutation during acquisition, public Release0 after all pins,
six typed families, exact failure prefixes, sticky errors, S_FALSE rejection,
null bindings, sequence exhaustion and complete reference cleanup. It includes the
prepared-lease baseline. It does not establish client immediate validation or real
paired/native backend behavior; those remain requirements before activation.
