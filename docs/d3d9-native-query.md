# Native query adapter

The service owns query registry IDs and serializes calls. Each native context owns
its real Query9, canonical IUnknown and parent device references. Creation compares
actual type/data size, validates GetDevice and supports genuine NULL-output support
probes. GetType/GetDataSize become immutable metadata for the COM frontend.

Issue and GetData forward flags unchanged. GetData uses aligned owned scratch
seeded from caller bytes and returns only the declared byte span for every HRESULT.
This preserves EVENT writes on S_FALSE and cached one-byte EVENT updates. NULL and
nonnull zero-size probes remain distinct. Oversized output requests fail before
calling a backend that could otherwise read past its cached result.

The actual PE64 DXVK fixture covers all five GPU query types, optional VCACHE,
BEGIN/END, S_FALSE while begun, bounded FLUSH polling, result retrieval, cached
partial writes, zero-size probes and clean device teardown in three processes.
No console or production-session integration is claimed by this fixture.
