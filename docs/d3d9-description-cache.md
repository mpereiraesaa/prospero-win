# Immutable texture description cache

Successful Texture2D GetLevelDesc replies are cached lazily in the proxy for the
same native object generation. Each level has its own entry; failures never
populate it. At most 32 levels are cached. Allocation failure or a larger level
uses the existing synchronous path without changing its result.

The native texture's dimensions, format, pool, usage and sample description do
not change during its lifetime. Lock, upload, mip generation, LOD and stateblock
operations change contents or device state, not these descriptions. A new native
generation creates a new proxy/cache. GetLevelCount and ordinary AddRef/nonfinal
Release were already local; this change removes repeated GetLevelDesc RPCs.

Surface GetDesc remains synchronous, including implicit backbuffer/depth surfaces
which can be reconciled during Reset. Their descriptors are not assumed immutable
across that reconciliation. Mutable device getters retain their ordered barriers.

Every cache hit retains the existing local method reference pin and frozen/closed
checks. Cache locks cover only local snapshots/publication; no RPC, output write
or retirement runs under them. Concurrent misses may issue duplicate reads of
the same immutable description. Final local destruction frees the cache.

The controlled PE32/PE64 proxy fixture proves separate level hits, exact returned
bytes, unchanged failure outputs, retry after a failed read, uncached invalid
levels, synchronous surfaces, and existing deferred destruction behavior. This
does not claim native backend or console performance acceptance.
