# Production D3D9 bridge smoke

`tests/lab/d3d9_game_smoke.py` consumes an already built PE32 proxy and matching
PE64 persistent service. It compiles only the guest client, freezes the supplied
pair, records input hashes, and executes three real DLL cycles. Supply a unique
initialized Wine prefix and the pinned Wine builtin PE32 `d3dx9_43.dll`.

The default client loads the unmodified compiled FX bytecode from Wine’s LGPL
`dlls/d3dx9_36/tests/effect.c` state fixture, attributed in its header. It also
uploads a real pixel shader and constants for the changed-pixel draw.

The client exercises D3DXCreateEffect (compiled vertex shader, matrix parameter upload,
Begin/BeginPass/CommitChanges/EndPass/End state restoration), D3DXCreateSphere,
and D3DXCreateTextureFromFileA. It draws an indexed triangle through copied VB/IB
locks, reads the render target through a copied surface lock, requires a red
center pixel, restores depth/render target bindings, and calls Reset/Present.
All three rows and a successful process exit are required. A timeout or SIGKILL
is a failure even if the rendering rows passed.

Use `--fullscreen` for the captured GTA SA presentation parameters: 1920x1080,
A8R8G8B8, FLIP, automatic D16, refresh zero. This fixture's HWND and hardware
vertex-processing flags are chosen inputs, not a claim about captured game flags.
`--direct-control` labels a run with a native PE32 DXVK DLL in place of the proxy;
it validates the fixture against the backend, without exercising the bridge.

Required runner arguments: `--wine-build`, `--prefix`, `--proxy`, `--service`,
`--backend64`, `--d3dx`, `--output`. This is host integration evidence; actual GTA SA
Proper Shaders and matched moving-route performance still require Claude's console
runs. Unsupported production methods must remain explicit failures.


`--source-effect` selects the separate check requiring builtin HLSL FX source compilation of a
pixel-shader effect. The pinned ordinary host Wine control currently rejects
its PixelShader assignment with E_NOTIMPL before any bridge involvement
(`/tmp/prospero-game-smoke-control-r6/receipt.json`). A previous NULL shader state
reached a vkd3d compiler unreachable loop; that diagnostic was terminated and
recorded as failed. The compiled FX fixture proves runtime COM and stateblock
behavior, not source compiler support. Actual Proper Shaders shader creation
and visible output must still be established on the console game run.
