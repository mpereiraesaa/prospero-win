# INTZ depth texture control and transport fixture

This fixture is designed to compare actual native INTZ support with the existing
bridge texture, command and object-getter paths. The ordinary PE64 control creates
a depth-stencil texture using FourCC INTZ, obtains its surface, binds it, clears
depth and restores the original D16 depth surface. The paired PE32/PE64 transport
then repeats creation, descriptor validation, canonical surface identity, binding,
depth clear and restoration through the production copied session.

Only the native window/device adapter is test scaffolding. It creates a small
windowed device with a real D16 depth surface; PS5 guest-window registration and
input are outside this fixture. All local headers, fixture/runtime sources and
artifacts are hashed. Direct file capture avoids waiting on inherited descendant
pipes after the client exits. Compile-only mode starts no Wine or GPU process and
reports `compiled-only`, never a runtime pass.

A fresh recovery run on 2026-10-09 passed three ordinary PE64 control cycles and
three typed transport cycles, both with exit code zero. The retained receipt
freezes 104 source/header hashes plus binaries and logs. It establishes the
creation, depth clear and restoration paths described above, not shader sampling,
Reset or console behavior. Earlier attempts failed during factory startup before
device creation and are not evidence of INTZ support or failure.
The optional `--disable-vr-host-control` is an explicitly recorded host diagnostic;
it must not be interpreted as a console profile or runtime change.
