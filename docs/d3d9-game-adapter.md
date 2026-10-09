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

The checked HELLO advertises all enabled method families. Proxy/service feature mismatches fail before publishing backend objects. Build receipts hash all shared headers as well as compiled source files.

Nine object getter vtable entries now publish exact backend references through typed canonical wrappers. Null bindings stay null; stream offset and stride are copied only after successful wrapping. The typed module pins the device across both getter transport and wrapper publication.

Surface container dispatch publishes the actual native Texture2D container through canonical adoption, or adds one guest reference to the checked exact parent device ID. Unsupported swapchain containers retain the explicit failure from native classification. Temporary backend references are released on every publication path.
