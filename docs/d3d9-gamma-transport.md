# Actual gamma session transport proof

The fixture exercises the production PE32 gamma COM methods, copied session
transport, pinned service-method dispatch and actual native DXVK Set/GetGammaRamp.
It uses an explicitly test-only ordinary native window and minimal local parent
shell instead of the PS5 association/device frontend.

Three cycles verify all channel values, Set flags, native NULL calls, unsupported
swapchain Get preserving the caller's initial bytes, restoration of the original
native ramp and clean session close. The receipt freezes every local wine/ps5
header and the exact C/runner sources and artifacts. This establishes host session
transport acceptance, not PS5 driver or console gamma behavior.

## Retained recovery proof

A fresh run after host runtime reconstruction passed three cycles with exit code
zero. The receipt retains all fixture/runtime source and header hashes, paired
binaries and logs in persistent storage. The earlier temporary receipts were lost
in the host reboot; the new result covers the same bounded host transport scope
above and does not establish console behavior.
