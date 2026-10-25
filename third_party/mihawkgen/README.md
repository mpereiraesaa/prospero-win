# MihawkGen (vendored)

The frame generator of [PS5 MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen)
by Mihawk: two colour frames in, the picture between them out, as Vulkan
compute passes recorded into the caller's command buffer. Its neural method
(0.43 M parameters, `models/network.fgw`) needs no data from the program.

- Source: `framegen/` of mihawk-99/PS5_MihawkGen at the commit in `COMMIT`,
  copied unchanged: `include/`, `src/` (with `src/shaders/`, the compute
  shaders as SPIR-V arrays) and `models/network.fgw`.
- Licence: GPL-3.0-or-later (`LICENSE.md`), used here with its author's
  permission. It is linked only into `libvulkan.prx`
  (`wine/ps5/pw_framegen.cpp`, `tools/link_radv_prx.sh`), which is
  distributed under the GPL as a whole already (`THIRD_PARTY.md`).
- Built with `wine/ps5/pw_framegen_vk.h` included first: the library calls
  Vulkan's functions by name, and inside the driver there is no loader, so
  the header turns each name into a pointer the shim loads from RADV.

To update: copy the same three parts of a newer commit and write its hash in
`COMMIT`.
