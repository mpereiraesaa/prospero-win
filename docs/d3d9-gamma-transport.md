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
