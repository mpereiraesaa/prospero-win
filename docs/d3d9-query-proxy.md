# Query COM frontend

Installs CreateQuery and implements the eight Query9 methods. QueryInterface
preserves local IUnknown identity; GetDevice returns the actual strong local parent.
GetType/GetDataSize use verified immutable metadata returned by native creation.
NULL-output CreateQuery remains a genuine remote support probe.

Issue and GetData pin their proxy across RPC. GetData copies caller bytes before
transport and publishes only a fully validated reply, preserving S_FALSE and
partial backend writes. Transport failures publish no output. Flags and NULL versus
nonnull zero-size outputs remain distinct. Output larger than actual GetDataSize
is rejected without transport.

A retirement shell is allocated before creating a remote object. Final Release
uses intrusive deferred cleanup when RPC is prohibited; failed enqueue cancels via
a nonblocking callback and retains parent/shell ownership. No lock spans RPC.

The controlled PE32/PE64 fixture exercises all eight methods, parent and interface
identity, true support probes, pending/success outputs, unchanged bytes, flags,
size rejection, repeated deferred release and failed deferral ownership. It does
not establish native backend, production-session or console acceptance.
