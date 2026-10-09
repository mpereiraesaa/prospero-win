# Default-surface public-reference lifetime differential

## Exact observed mismatch

Installed GTA RenderWare stores GetRenderTarget(0)/GetDepthStencilSurface results in C97C30/C97C2C, immediately releases each returned reference at 7F9A3E/7F9A5C, and later passes the retained addresses to SetRenderTarget/SetDepthStencilSurface. R4 returns nonnull surfaces at capture sequences33920/33921 and fails the default-target restore at33930. ProperShaders forwards or substitutes the depth surface; its exact selected pointer remains unlogged.

The focused fixture repeats that sequence against pinned native DXVK and the full production PE32 proxy→session→PE64 service. It runs before and after successful Reset in three device cycles. Native returns Release0 and both Set calls succeed in all six cases. The unfixed bridge returns Release0 and both Set calls return8876086c in all six cases. Reacquiring owned getter references then binding succeeds on both paths. Device/factory final Release and both process exits are clean.

The bridge removes/frees the local surface shell at public refcount0, while its resolver only recognizes live cache entries. Native implicit/private ownership keeps the corresponding backend default surface alive. This is a demonstrated compatibility defect, not an expected NULL-cleanup rejection. Attribution of all three console calls still has limits: their individual resolver outcome/refcounts and the ProperShaders replacement pointer were not logged.

## Retained evidence

- Production source base: `64aeaf73747fc07df68ff19617ae3ddac45ac4ee`. No shipping source is modified by this fixture.
- Persistent root: `../prospero-win-artifacts/bridge-recovery-20261009/`.
- `surface-lifetime-r2/receipt.json`: status pass, seven terminal-zero commands, 168 source/header hashes, native/backend artifact hashes and exact compile/run commands. SHA256 `c1faa73ca754b5fbefde5143b10ee5064943caa1be97792b2f9d5b1c408bb6e0`.
- `surface-lifetime-r2/native.stdout` and `bridge.stdout`: six rows each and terminal `SURFACE_CLOSE status=0`.
- `gta-bindings-analysis-r4/`: exact game/ASI hashes and caller/getter/release disassemblies.
- r1 failed before any compiler or runtime launch due to source-path normalization; retained receipt labels that failure.

The test-only native device adapter substitutes an ordinary X11 HWND for PS5 association and enables real automatic D16 depth. All shipping proxy, registry, codecs, resource helpers and command dispatch remain intact. This proves the lifetime difference through actual DXVK, not PS5 window association, console rendering or whole-game recovery. No pre-Reset released pointer is reused after Reset. Address equality on getter reacquisition is reported but is not a lifetime assertion: the bridge allocator reused freed addresses in this run.

## Run

The runner consumes the retained production build recipe at `RECOVERY/production-guest-fullscreen-r1/receipt.json`, rebuilding against the current fixture tree and replacing only the native-window adapter. It requires recovered host runtime/backend and copies the initialized prefix into its own output. Run under the single coordinated 8GiB slot:

```sh
systemd-run --user --scope -p MemoryMax=8G -p MemorySwapMax=0 python3 tests/lab/d3d9_surface_lifetime.py --recovery /absolute/recovery --output /absolute/new-proof
```

The default runner requires successful native/bridge parity. `--expect-proxy-failure` selects the explicit unfixed negative control expecting bridge INVALIDCALL. The retained r2 proof used the frozen pre-parity-switch source in commit `76af14d0`; its original 168 hashes and binaries remain immutable. The positive proof below validates the repaired ownership implementation. Both variants require owned reacquisition to bind successfully and final device/factory Release to return zero across all three cycles.

## Positive Reset and controlled-owner coverage

The expanded positive fixture adds early invalid SwapEffect rejection with an unbound automatic depth surface, late NotReset rejection from an explicit live DEFAULT render target, and publicly held implicit surfaces that remain usable after blocking Reset. It releases the blocker/public references and requires successful recovery. Old public-zero addresses are never used through a surface COM vtable after retirement.

A fixture-only high presentation flag is removed before the real native Reset and injects one post-native mirror failure. The adapter captures `reset_preserved` from the actual backend outcome before this injection. The final bridge reply is DEVICELOST, but the address-only SetRenderTarget resolver must reject the retired old handle. This does not simulate an actual PS5 window operation.

`--production-root /path/to/frozen/integration` compiles the candidate shipping sources while retaining these fixture sources separately in the hash map. `--owner-control` additionally runs both PE32 and PE64 controlled-owner fixtures covering frozen resurrection refusal, early thaw identity, positive-public release during Reset, exactly-once native retirement, and final device-close admission. These cases now pass in `surface-lifetime-positive-r2`; the earlier `surface-lifetime-r2` directory remains the original unfixed differential.


### Fresh positive receipt

- Shipping integration `03704b34589926ff0f4e5cd7039690f6976b2a5f`; fixture source `b05073d4`.
- `surface-lifetime-positive-r2/receipt.json`: PASS, 11 terminal-zero commands, 171 source/header hashes. SHA256 `fca9fd4128489261583b2f4e510958e59d6f1a8821673edabe50a33fc90cd699`.
- Native and bridge each: six RT and six depth binds succeed after Release0, same getter identity, three cycles covering pre/post Reset.
- Both paths: three early invalid-parameter failures restore borrowed RT and unbound auto-depth identity; three late NotReset failures retire implicit owners and recover after the DEFAULT blocker is released; three publicly held implicit-surface failures preserve usable GetDesc until release and subsequent successful Reset.
- Bridge: three successful native Reset calls followed by fixture-injected mirror failure still retire old zero-public handles despite the final DEVICELOST reply.
- Both controlled PE ABIs pass frozen AddRef/QI/resolve refusal, early thaw, positive-reference drop during Reset, and final closing-gate/disposal assertions. Every actual device and factory final Release returns zero, and both actual backend processes close cleanly.
- `verification.json` independently matches all 171 captured inputs to their exact shipping/fixture commits and verifies all six PE artifact hashes.

`surface-lifetime-positive-r1` failed during strict PE32 compilation due to misleading-indentation warnings in new owner code; no runtime launched. The subsequent formatting-only shipping fix and test-adapter actual presentation-parameter copyback are reflected in the positive-r2 frozen hashes. No failed receipt is relabeled successful.

Independent earlier controlled proof `surface-owners-control-r1/receipt.json` passed both ABIs against owner implementation `e6de11c9` and fixture `06b106a7`: 88 verified inputs, five terminal-zero commands, SHA256 `d5a16c8e92a3e1673c720073c9c1cdae95fd2495335aad5478e5294249260344`. Its scope note clarifies that the fixture loads no D3D backend, while Wine prefix initialization may probe EGL; the original receipt remains immutable.
