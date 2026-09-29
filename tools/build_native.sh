#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the prospero-win PS5 title: the launcher and Wine's own Unix side
# (ntdll.prx) started in-process (native/wine64_main.c). The Wine runtime
# itself comes from tools/build_wine_ps5.sh and is staged beside the title.
#
# Environment:
#   PS5_NATIVE_FOUNDATION  boilerplate checkout (default .deps/, pinned)
#   PS5LOG_DEV_CONF        private dev.conf copied into the title
#   PW_FOUNDATION_READY    1 trusts an already prepared foundation checkout
#                          and verifies its artifacts instead of rebuilding
#                          its dependencies, which would mutate a tree the
#                          laboratory's other projects share (default 0)
#   PW_NATIVE_MODE         wine64, the only mode (accepted for older scripts)
#   PW_OUTPUT_SUFFIX       isolated build/dist suffix, e.g. -wine-smoke
#   PW_WINE64_SCRIPT       1 drives the launcher unattended (validation)
#   PW_WINE64_SECONDS      close each game after this many seconds; 0 never
#   PW_WINE64_SCRIPT_CYCLES games the unattended launcher opens (default 2)
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
pin=37dd53602bdead63936f718004555ba10154be48
url=https://github.com/mpereiraesaa/ps5-native-app-boilerplate.git
foundation=${PS5_NATIVE_FOUNDATION:-$root/.deps/ps5-native-app-boilerplate}
dev_conf=${PS5LOG_DEV_CONF:-$root/dev.conf}
native_mode=${PW_NATIVE_MODE:-wine64}
output_suffix=${PW_OUTPUT_SUFFIX:-}
wine64_script=${PW_WINE64_SCRIPT:-0}
wine64_seconds=${PW_WINE64_SECONDS:-0}
wine64_cycles=${PW_WINE64_SCRIPT_CYCLES:-2}

[[ $native_mode == wine64 ]] || {
    echo "PW_NATIVE_MODE must be wine64: the direct Win32 runtime was removed" >&2; exit 2; }
[[ $wine64_script == 0 || $wine64_script == 1 ]] && [[ $wine64_seconds =~ ^[0-9]+$ ]] &&
    [[ $wine64_cycles =~ ^[1-9][0-9]{0,3}$ ]] || {
    echo "PW_WINE64_SCRIPT must be 0 or 1, PW_WINE64_SECONDS a number and" \
         "PW_WINE64_SCRIPT_CYCLES 1-9999" >&2; exit 2; }
[[ $output_suffix =~ ^[A-Za-z0-9_-]*$ ]] || {
    echo "PW_OUTPUT_SUFFIX must contain only letters, digits, '_' or '-'" >&2
    exit 2
}

if [[ ! -d $foundation/.git ]]; then
    mkdir -p -- "$(dirname -- "$foundation")"
    git clone --filter=blob:none "$url" "$foundation"
fi
actual=$(git -C "$foundation" rev-parse HEAD)
if [[ $actual != "$pin" ]]; then
    git -C "$foundation" fetch origin "$pin"
    git -C "$foundation" checkout --detach "$pin"
fi
[[ $(git -C "$foundation" rev-parse HEAD) == "$pin" ]] || {
    echo "native foundation pin verification failed" >&2; exit 2; }

sdk="$foundation/.deps/native/ps5-payload-sdk"
native="$foundation/tooling/native"
tool="$foundation/build/host/ps5-native-tool"

# The pinned foundation is often a checkout shared with the laboratory's
# other projects. Rebuilding its dependencies mutates that tree and reaches
# the network, so when it is already complete, verify it instead.
if [[ ${PW_FOUNDATION_READY:-0} == 1 ]]; then
    for artifact in "$sdk/bin/prospero-lld" "$sdk/target/lib/libkernel.so" \
                    "$foundation/runtime/libc.prx" \
                    "$native/ps5-pie.ld" "$native/app_crt.cpp"; do
        [[ -e $artifact ]] || {
            echo "PW_FOUNDATION_READY=1 but $artifact is missing" >&2
            exit 2
        }
    done
    echo "using the prepared foundation at $foundation (deps not rebuilt)"
else
    make -C "$foundation" deps libc >/dev/null
fi
if [[ ! -x $tool ]]; then
    zlib_root="$foundation/.deps/native/zlib/root"
    zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit)
    cxx=$(command -v clang++-18 || command -v clang++ || true)
    [[ -n $cxx && -n $zlib_archive ]] || {
        echo "native foundation host-tool dependencies are unavailable" >&2
        exit 2
    }
    mkdir -p "$foundation/build/host"
    "$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
        -I "$zlib_root/usr/include" \
        "$native/native_app_builder.cpp" "$native/self_container.cpp" \
        "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
        "$zlib_archive" -o "$tool"
fi
[[ -x $tool && -d $sdk && -f $foundation/runtime/libc.prx ]] || {
    echo "native foundation did not produce its SDK, tool and runtime" >&2
    exit 2
}

title_id=PPSA99995
build="$root/build/native$output_suffix"
dist="$root/dist/$title_id$output_suffix"
rm -rf -- "$build" "$dist"
mkdir -p "$build/obj" "$build/import-stubs" "$dist/sce_sys" "$dist/sce_module"

cc=(env PS5_PAYLOAD_SDK="$sdk" sh "$foundation/tooling/prospero-clang18")
common=(-O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
        -I"$root/include" -I"$root/src" -I"$root/native"
        -I"$root/native/ps5log"
        -DPW_WINE64_SCRIPT="$wine64_script" -DPW_WINE64_SECONDS="$wine64_seconds"
        -DPW_WINE64_SCRIPT_CYCLES="$wine64_cycles")

sources=(
    native/wine64_main.c native/pw_audio_ps5.c native/pw_pad_ps5.c native/pw_agc_ps5.c
    native/pw_agc_submit_lifecycle.c native/pw_videoout_ps5.c native/pw_data_mount.c
    native/pw_wine_display.c native/pw_wine_library.c native/pw_hid_ps5.c
    src/pw_result.c src/pw_wine_start.c src/pw_wine_launch.c src/pw_game_profile.c
    src/pw_app_profile.c src/pw_profile_catalog.c src/pw_launcher_render.c src/pw_present.c
    src/pw_pad.c src/pw_hid.c src/pw_spinner.c wine/ps5/pw_wine_prx.c
)
objects=()
for source in "${sources[@]}"; do
    object="$build/obj/${source//\//_}.o"
    "${cc[@]}" -std=c11 "${common[@]}" -c "$root/$source" -o "$object"
    objects+=("$object")
done
"${cc[@]}" -std=c11 "${common[@]}" \
    -include "$root/native/ps5log/ps5log_ps5_net.h" \
    -c "$root/native/ps5log/ps5log.c" -o "$build/obj/ps5log.o"
"${cc[@]}" -std=c11 "${common[@]}" \
    -c "$root/native/ps5log/ps5log_ps5_net.c" \
    -o "$build/obj/ps5log_ps5_net.o"
"${cc[@]}" -std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" \
    -o "$build/obj/app_crt.o"
objects+=("$build/obj/ps5log.o" "$build/obj/ps5log_ps5_net.o")

# VideoOut flips through AGC; these link-only facades name its imports.
"${cc[@]}" -std=c11 -O2 -fPIC -c "$root/native/stubs/libSceAgc.c" \
    -o "$build/obj/agc-import.o"
"$sdk/bin/prospero-lld" --shared -soname libSceAgc.prx \
    -o "$build/import-stubs/libSceAgc.so" "$build/obj/agc-import.o"
"${cc[@]}" -std=c11 -O2 -fPIC -c "$root/native/stubs/libSceAgcDriver.c" \
    -o "$build/obj/agc-driver-import.o"
"$sdk/bin/prospero-lld" --shared -soname libSceAgcDriver.prx \
    -o "$build/import-stubs/libSceAgcDriver.so" "$build/obj/agc-driver-import.o"

"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr -e _start \
    -o "$build/llvm-pie.elf" "$build/obj/app_crt.o" "${objects[@]}" \
    --as-needed "$sdk"/target/lib/*.so "$build/import-stubs/libSceAgc.so" \
    "$build/import-stubs/libSceAgcDriver.so"
"$tool" link --in "$build/llvm-pie.elf" --out "$build/eboot.elf" \
    --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 \
    --stub "$build/import-stubs/libSceAgc.so" \
    --stub "$build/import-stubs/libSceAgcDriver.so" \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$build/eboot.elf" --out "$dist/eboot.bin" \
    --magic 0x1D3D154F
# The console installer copies icon0.png into /user/app/<title> and aborts
# the whole registration if it is absent, so it is not optional.
[[ -f $root/sce_sys/icon0.png ]] || python3 "$root/tools/make_icon.py"
cp "$root/sce_sys/param.json" "$root/sce_sys/icon0.png" "$dist/sce_sys/"
cp "$foundation/runtime/libc.prx" "$dist/sce_module/libc.prx"
if [[ -f $dev_conf ]]; then
    cp "$dev_conf" "$dist/dev.conf"
fi

# Every dynamic import in the linked ELF is reviewed, per the porting
# playbook: an exported platform symbol is not a working one.
readelf=${LLVM_READELF:-$(command -v llvm-readelf-18 || command -v llvm-readelf || true)}
if [[ -n $readelf && -x $readelf ]]; then
    "$readelf" --dyn-syms "$build/llvm-pie.elf" \
        | awk '$7 == "UND" { print $8 }' | sort -u \
        > "$build/PW_DYNAMIC_IMPORTS.txt"
    if grep -qx 'strcasestr' "$build/PW_DYNAMIC_IMPORTS.txt"; then
        echo "strcasestr is banned: its provider is unusable on FW 12.02" >&2
        exit 2
    fi
    echo "dynamic imports recorded in $build/PW_DYNAMIC_IMPORTS.txt"
else
    echo "llvm-readelf unavailable: dynamic-import review skipped" >&2
fi

sha256sum "$build/eboot.elf" "$dist/eboot.bin"
