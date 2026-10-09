# PE32 D3D9 COM factory proxy

`wine/ps5/d3d9/pw_d3d9_proxy.c` exports Direct3DCreate9 and wraps the
[persistent native service](d3d9-persistent-session.md) in a PE32 IDirect3D9
vtable. Its local COM pointers never leave the client. The backend object ID
and generation identify the corresponding native factory.

IUnknown and IDirect3D9 QueryInterface return the same local identity and
increment the local reference count. The client cache uses the complete remote
ID/generation pair; a repeated returned reference is folded into that canonical
proxy while balancing the extra remote reference. Final local Release releases
the remote factory. The final factory also stops and joins the service. The
DLL performs no blocking cleanup from DllMain. Applications must release their
objects before unloading the proxy module, as required for live COM vtables.

Slots4–14 call the native backend through explicit field codecs. Output structs
are assigned only after a successful reply. Count methods return zero on
transport failure. RegisterSoftwareDevice and CreateDevice explicitly return
D3DERR_NOTAVAILABLE; GetAdapterMonitor returns NULL. Direct3DCreate9Ex is
exported but returns D3DERR_NOTAVAILABLE and a null object. IDirect3D9Ex
QueryInterface returns E_NOINTERFACE. Device/window support must be implemented
before this DLL can be enabled for a game.

For the focused fixture, `PW_D3D9_SERVICE64` and `PW_D3D9_BACKEND64` specify
absolute Windows paths. They are read on the PE32 side before the service
starts; no environment update needs to propagate across domains. Production
launcher/config integration remains separate.

Run `tests/lab/d3d9_factory_proxy.py` with `--wine-build`, `--prefix`,
`--backend64` and a fresh `--output`. It builds the proxy, native service and
PE32 client, records their source/binary hashes, and runs three complete
sessions. Each checks canonical identity, all11 supported factory methods,
failed output preservation, null output rejection, unsupported exports/methods,
multiple factories, independent release and final service teardown.


GetAdapterMonitor maps the real backend adapter identifier DeviceName to a guest
monitor using EnumDisplayMonitors/GetMonitorInfoA. The returned HMONITOR is owned
by the guest display system and never crosses the bridge wire. Invalid adapters,
empty or unterminated names, and missing guest matches return NULL. No default
monitor is substituted for an unmatched native adapter. The actual backend
fixture checks a valid guest monitor's name and invalid adapter rejection.

Profile-compatible forward-slash environment paths are normalized to Windows backslashes before session startup. Drive-absolute validation remains in the session/bootstrap; relative paths still fail. The actual factory fixture supplies forward-slash paths to cover this launch path.
