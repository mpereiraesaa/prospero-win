# Production D3D9 adapter assembly

The paired build enables device operations, typed command/getter methods, buffers,
programs with native readback, textures/surfaces, stateblocks and object getters.
Each frontend callback temporarily pins its parent device across RPC and message
pumping. No global identity lock spans transport calls. Remote references returned
by getters are consumed by canonical local wrappers.

Build with `python3 tests/lab/d3d9_game_build.py --output <new-directory>`.
The receipt records source/header hashes and both binary hashes. Build success
is not console acceptance. Deploy both binaries and the matching native-domain
runtime together; the service backend remains the pinned DXVK build.

Current omissions include UP draws, cube/volume resources, queries and surface DC
operations. Unsupported methods report failure; void/value-return methods also
make the session failure sticky. GTA SA and its D3DX effects still require actual
production-DLL smoke and console validation.
