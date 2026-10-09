# Prepared binding leases (inactive)

The helper owns a complete decoded command batch, validates every method's
scalar/canonical eligibility, then pins every valid referenced registry generation
before invoking any native acquire callback. It retains one typed native COM lease
per non-null binding record and its registry pin until explicit release. Duplicate
references are bounded by the 128-record batch limit and balanced individually.
The caller must already hold the target device's generation pin through cleanup.

The ordinary scalar policy and shipping transport are unchanged. Preparing this
object does not enable asynchronous binding acceptance. The client must first
reject ordinary stale, wrong-kind and cross-device inputs immediately through an
atomic typed lifetime-ticket admission path. A later service target failure is
channel-fatal fallback, not a substitute for the ordinary immediate HRESULT.

Typed preparation failures are saved per record. Execute commands exclusively
from the helper's owned batch, passing that record's lease as native dispatcher
acquire context. When execution reaches a bad target it receives its actual saved
failure, so earlier commands form the real executed prefix. Later commands must
not run. A positive non-S_OK acquire result is recorded and converted to E_FAIL,
never silently accepted. Native command HRESULT handling remains the ordered batch
executor's responsibility. Policy/byte errors reject before all pins/execution.

Success, first failure, or cancellation must release every lease, including the
unexecuted suffix, before dropping the device pin or draining native destruction.
Registry cancellation preserves queued references, so native objects remain alive
until this release. Serialize registry operations on the service owner; do not
hold a registry mutex across native COM callbacks. Destroying the registry before
releasing prepared leases violates the ownership contract.

The controlled fixture uses real registry, codecs, policy and native dispatcher
with mock native COM objects. It checks all six binding kinds, nulls, duplicates,
public Release0 before execution, Release0 during the first acquire callback,
owned-copy isolation, exact failure prefixes, typed validation, unexpected S_FALSE,
unexecuted-suffix cleanup and cancellation. Every owned native reference reaches
zero. It does not prove real DXVK, client publication tickets, paired transport or
console behavior. Shipping admission remains disabled pending those proofs.
