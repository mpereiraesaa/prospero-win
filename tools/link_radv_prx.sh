#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# RADV's libvulkan.prx for Wine (tools/build_wine_ps5.sh --radv). The driver
# is Mesa's release archive, built by a PS5_Vulkan checkout
# (tools/build-radv.sh release) from its pinned PS5_Mesa revision, linked with
# that checkout's own recipe (tools/radv-link.sh: the platform layer's heap,
# threads and libc) and its payload SDK, behind wine/ps5/pw_vulkan_radv.c's
# two entry points. It writes PRX/libvulkan.shared.elf, the link log
# PRX/libvulkan.link.log and PRX/sce_module/libvulkan.prx, and prints the
# PS5_Mesa revision it linked. RADV's sceVideoOutOpen is wrapped so
# pw_vulkan_radv.c learns the output's handle for the hardware cursor.
#
# Usage: tools/link_radv_prx.sh RADV_CHECKOUT PRX_DIR NATIVE_TOOL PIE_SCRIPT
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
radv=$1 prx=$2 tool=$3 pie=$4
sdk=$radv/.deps/native/ps5-payload-sdk
install=$radv/.deps/native/radv-release
archive=$install/lib/libvulkan_radeon.ps5.a
work=$prx/radv
log=$prx/libvulkan.link.log
revision=$(sed -n 's/^revision: //p' "$install/PROVENANCE.txt" 2>/dev/null || true)
[[ -f $archive && -n $revision ]] ||
    { echo "no RADV release archive in $install: run tools/build-radv.sh release there" >&2; exit 2; }
[[ -f $radv/tools/radv-link.sh ]] || { echo "$radv has no tools/radv-link.sh" >&2; exit 2; }

mkdir -p "$work/obj" "$work/stubs" "$work/build" "$prx/sce_module"
# shellcheck source=/dev/null
source "$radv/tools/radv-link.sh"
radv_link_recipe "$work" "$sdk" "$archive"

# Mesa's dispatch tables reference optional entry points weakly. In a shared
# ELF lld would leave each as a dynamic import, which the PRX converter takes
# for a required external function: resolve them to 0, as an executable would.
"$sdk/bin/llvm-nm" -g "$archive" | awk '$1 == "w" {print $2}' | LC_ALL=C sort -u |
    awk '{print "PROVIDE_HIDDEN(" $0 " = 0);"}' > "$work/build/mesa_optional_zero.ld"

# The AGC libraries' link stubs, from the checkout's vendored stub sources.
for name in libSceAgc libSceAgcDriver; do
    case $name in
    libSceAgc) source_file=$radv/vendor/ps5/sdk/stubs/agc_canary_link_stub.c ;;
    libSceAgcDriver) source_file=$radv/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c ;;
    esac
    "$sdk/bin/prospero-clang" -std=c11 -O2 -fPIC -c "$source_file" -o "$work/obj/${name}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "$name.prx" -o "$work/stubs/$name.so" "$work/obj/${name}_stub.o"
done
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$install/include" -I"$root/wine/ps5" \
    -c "$root/wine/ps5/pw_vulkan_radv.c" -o "$work/obj/pw_vulkan_radv.o"
# The entry-point profile behind the shim (wine/ps5/pw_vk_radv_profile.h).
for unit in pw_vk_radv_profile pw_vk_radv_profile_wrap; do
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$install/include" -I"$root/wine/ps5" \
        -c "$root/wine/ps5/$unit.c" -o "$work/obj/$unit.o"
done
python3 "$root/tools/gen_prx_descriptor.py" "$work/obj/libvulkan_desc.c" \
    vkGetInstanceProcAddr vkGetDeviceProcAddr pw_videoout_idle pw_videoout_show_tiled pw_videoout_cursor \
    pw_vk_radv_profile_set_frame_hook
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$root/wine/ps5" \
    -c "$work/obj/libvulkan_desc.c" -o "$work/obj/libvulkan_desc.o"

"$sdk/bin/prospero-lld" --shared -Bsymbolic -T "$pie" -T "$root/wine/ps5/prx_eh_frame.ld" \
    --eh-frame-hdr -T "$work/build/mesa_optional_zero.ld" -soname libvulkan.prx -z defs \
    --wrap=sceVideoOutOpen \
    --warn-unresolved-symbols -o "$prx/libvulkan.shared.elf" \
    "$work/obj/pw_vulkan_radv.o" "$work/obj/pw_vk_radv_profile.o" "$work/obj/pw_vk_radv_profile_wrap.o" \
    "$work/obj/libvulkan_desc.o" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_flags[@]}" "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so > "$log" 2>&1
"$tool" link --module --in "$prx/libvulkan.shared.elf" --out "$work/libvulkan.elf" \
    --stub-dir "$sdk/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name libvulkan.prx >> "$log" 2>&1
"$tool" self --sign --in "$work/libvulkan.elf" --out "$prx/sce_module/libvulkan.prx" >> "$log" 2>&1
echo "$revision"
