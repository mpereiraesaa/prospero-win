#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the prospero-win native PS5 title. Runtime is the default; the
# non-executing PE mapping validator remains available explicitly.
#
# The mapping validator needs no shaders or GPU: it maps Windows images and
# reports their dependency graph through ps5log/1. Staged Windows binaries
# come from an operator-provided private path and are never committed.
#
# Environment:
#   PS5_NATIVE_FOUNDATION  boilerplate checkout (default .deps/, pinned)
#   HOST_CC                host C compiler for the profile stage preflight
#   PS5LOG_DEV_CONF        private dev.conf copied into the title
#   PW_STAGE_INPUT         private directory holding the PE images to stage
#   PW_ROOT_MODULE         root image (default sample.exe, or app.exe for wine)
#   PW_APP_PROFILE         optional app.profile manifest for profile launch
#   PW_WINE_RUNTIME_DIR    optional pinned Wine distribution packaged under
#                          win/runtime; this does not select the Wine runner
#   PW_SAMPLE              1 stages generated synthetic images instead of a
#                          private directory (default 0)
#   PW_COMPAT32_TRANSFER   1 attempts an experimental far transfer into 32-bit
#                          compatibility mode (default 0: install and report
#                          the descriptors only, which cannot fault)
#   PW_FOUNDATION_READY    1 trusts an already prepared foundation checkout
#                          and verifies its artifacts instead of rebuilding
#                          its dependencies, which would mutate a tree the
#                          laboratory's other projects share (default 0)
#   PW_NATIVE_MODE         runtime (default), gate, or wine bootstrap
#   PW_OUTPUT_SUFFIX       isolated build/dist suffix, e.g. -wine-smoke
#   PW_TEST_EXIT_AFTER_MS  validation-only orderly runtime exit; 0 disables it
#   PW_DBT_CHAINING        1 enables direct block chaining (default 1)
#   PW_DBT_RESIDENCY       1 enables cross-block guest GPR residency (default 1)
#   PW_DBT_LAZY_FLAGS      1 enables cross-block RAW flag deferral (default 1)
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
pin=37dd53602bdead63936f718004555ba10154be48
url=https://github.com/mpereiraesaa/ps5-native-app-boilerplate.git
foundation=${PS5_NATIVE_FOUNDATION:-$root/.deps/ps5-native-app-boilerplate}
dev_conf=${PS5LOG_DEV_CONF:-$root/dev.conf}
stage_input=${PW_STAGE_INPUT:-}
root_module=${PW_ROOT_MODULE:-}
app_profile=${PW_APP_PROFILE:-}
wine_runtime_dir=${PW_WINE_RUNTIME_DIR:-}
use_app_profile=0
use_sample=${PW_SAMPLE:-0}
compat32_transfer=${PW_COMPAT32_TRANSFER:-0}
native_mode=${PW_NATIVE_MODE:-runtime}
test_exit_after_ms=${PW_TEST_EXIT_AFTER_MS:-0}
dbt_chaining=${PW_DBT_CHAINING:-1}
dbt_residency=${PW_DBT_RESIDENCY:-1}
dbt_lazy_flags=${PW_DBT_LAZY_FLAGS:-1}
output_suffix=${PW_OUTPUT_SUFFIX:-}

[[ $use_sample == 0 || $use_sample == 1 ]] || {
    echo "PW_SAMPLE must be 0 or 1" >&2; exit 2; }
if [[ $native_mode == wine && -z $app_profile ]]; then
    if [[ $use_sample == 1 ]]; then
        app_profile="$root/examples/profiles/wine-sample.profile"
    else
        echo "PW_APP_PROFILE is required for PW_NATIVE_MODE=wine" >&2
        exit 2
    fi
fi
[[ $compat32_transfer == 0 || $compat32_transfer == 1 ]] || {
    echo "PW_COMPAT32_TRANSFER must be 0 or 1" >&2; exit 2; }
[[ $native_mode == runtime || $native_mode == gate || $native_mode == wine ]] || {
    echo "PW_NATIVE_MODE must be runtime, gate or wine" >&2; exit 2; }
if [[ -z $root_module ]]; then
    if [[ $native_mode == wine ]]; then root_module=app.exe
    else root_module=sample.exe
    fi
fi
[[ $output_suffix =~ ^[A-Za-z0-9_-]*$ ]] || {
    echo "PW_OUTPUT_SUFFIX must contain only letters, digits, '_' or '-'" >&2
    exit 2
}
[[ $test_exit_after_ms =~ ^[0-9]+$ && $test_exit_after_ms -le 600000 ]] || {
    echo "PW_TEST_EXIT_AFTER_MS must be an integer from 0 to 600000" >&2; exit 2; }
for value in "$dbt_chaining" "$dbt_residency" "$dbt_lazy_flags"; do
    [[ $value == 0 || $value == 1 ]] || {
        echo "PW_DBT_CHAINING, PW_DBT_RESIDENCY and PW_DBT_LAZY_FLAGS must be 0 or 1" >&2
        exit 2
    }
done
[[ $root_module =~ ^[A-Za-z0-9_.-]+$ ]] || {
    echo "PW_ROOT_MODULE must be a bare file name" >&2; exit 2; }
if [[ -n $app_profile ]]; then
    [[ -f $app_profile ]] || { echo "PW_APP_PROFILE must name a file" >&2; exit 2; }
    profile_bytes=$(wc -c < "$app_profile")
    (( profile_bytes > 0 && profile_bytes <= 8192 )) || {
        echo "PW_APP_PROFILE must be between 1 and 8192 bytes" >&2; exit 2; }
    use_app_profile=1
fi
if [[ $native_mode == wine && -z $wine_runtime_dir ]]; then
    echo "PW_WINE_RUNTIME_DIR is required for PW_NATIVE_MODE=wine" >&2; exit 2
fi
if [[ $use_sample == 0 && -z $stage_input ]]; then
    echo "PW_STAGE_INPUT or PW_SAMPLE=1 is required" >&2; exit 2
fi
if [[ $use_sample == 0 && ! -d $stage_input ]]; then
    echo "PW_STAGE_INPUT must name a directory" >&2; exit 2
fi

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
mkdir -p "$build/obj" "$build/import-stubs" "$dist/sce_sys" "$dist/sce_module" "$dist/win"

# Stage and validate guest inputs before the costly native link. A profile
# whose executable is absent must fail here, not after producing a title that
# can only abort at runtime.
if [[ $native_mode == wine ]]; then
    mkdir -p "$dist/win/app"
    if [[ $use_sample == 1 ]]; then
        [[ $root_module == app.exe ]] || {
            echo "PW_SAMPLE=1 in wine mode requires PW_ROOT_MODULE=app.exe" >&2
            exit 2
        }
        python3 "$root/tools/make_test_pe.py" --application \
            --out-dir "$dist/win/app"
    else
        python3 "$root/tools/stage_app_files.py" \
            "$stage_input" "$dist/win/app"
    fi
    cp -- "$app_profile" "$dist/win/app/app.profile"
    "${HOST_CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
        "$root/tools/validate_profile_stage.c" "$root/src/pw_app_profile.c" \
        -o "$build/validate_profile_stage"
    "$build/validate_profile_stage" \
        "$dist/win/app/app.profile" "$dist/win/app"
    root_module=$("$build/validate_profile_stage" \
        "$dist/win/app/app.profile" "$dist/win/app" --root-module)
    [[ $root_module =~ ^[A-Za-z0-9_.-]+$ ]] || {
        echo "app.profile selected an invalid Wine root module" >&2; exit 2; }
    [[ -f $dist/win/app/$root_module ]] || {
        echo "Wine root module $root_module is not staged in $dist/win/app" >&2
        exit 2
    }
elif [[ $use_sample == 1 ]]; then
    python3 "$root/tools/make_test_pe.py" --out-dir "$dist/win"
else
    shopt -s nullglob
    for source in "$stage_input"/*; do
        [[ -f $source ]] || continue
        name=$(basename -- "$source")
        cp -- "$source" "$dist/win/${name,,}"
    done
    shopt -u nullglob
fi
if (( use_app_profile && use_sample == 0 )) && [[ $native_mode != wine ]]; then
    python3 "$root/tools/stage_app_files.py" "$stage_input" "$dist/win/app"
fi
if (( use_app_profile )) && [[ $native_mode != wine ]]; then
    cp -- "$app_profile" "$dist/win/app.profile"
    "${HOST_CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
        "$root/tools/validate_profile_stage.c" "$root/src/pw_app_profile.c" \
        -o "$build/validate_profile_stage"
    "$build/validate_profile_stage" "$dist/win/app.profile" "$dist/win"
elif [[ $native_mode != wine ]]; then
    [[ -f $dist/win/$root_module ]] || {
        echo "root module $root_module is not staged in $dist/win" >&2; exit 2; }
fi
if [[ -n $wine_runtime_dir ]]; then
    [[ -d $wine_runtime_dir ]] || {
        echo "PW_WINE_RUNTIME_DIR must name a staged Wine distribution" >&2
        exit 2
    }
    python3 "$root/tools/stage_wine_runtime.py" \
        --source "$wine_runtime_dir" \
        --destination "$dist/win/runtime"
fi
wine_version=11.17
if [[ $native_mode == wine ]]; then
    wine_version=$(python3 - "$dist/win/runtime/wine-runtime-manifest.json" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    version = json.load(stream)["wine"]["version"]
if version.startswith("Wine version "):
    version = version[len("Wine version "):]
if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", version):
    raise SystemExit("staged Wine manifest has an invalid version")
print(version)
PY
    )
fi

cc=(env PS5_PAYLOAD_SDK="$sdk" sh "$foundation/tooling/prospero-clang18")
common=(-O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
        -I"$root/include" -I"$root/src" -I"$root/native"
        -I"$root/native/ps5log"
        -DPW_STAGE_DIR='"/app0/win"'
        -DPW_ROOT_MODULE="\"$root_module\""
        -DPW_WINE_VERSION="\"$wine_version\""
        -DPW_USE_APP_PROFILE="$use_app_profile"
        -DPW_TEST_EXIT_AFTER_MS="$test_exit_after_ms"
        -DPW_DBT_CHAINING="$dbt_chaining"
        -DPW_DBT_RESIDENCY="$dbt_residency"
        -DPW_DBT_LAZY_FLAGS="$dbt_lazy_flags"
        -DPW_COMPAT32_TRANSFER="$compat32_transfer")

entry=native/runtime_main.c
[[ $native_mode == gate ]] && entry=native/main.c
[[ $native_mode == wine ]] && entry=native/wine_main.c
sources=(
    "$entry" native/pw_file_ps5.c native/pw_prefix_ps5.c native/pw_audio_ps5.c native/pw_pad_ps5.c native/pw_state_ps5.c native/pw_agc_ps5.c native/pw_agc_submit_lifecycle.c native/pw_videoout_ps5.c native/pw_compat32_ps5.c
    native/pw_lowmem_ps5.c
    src/pe_image.c src/pe_import.c src/pe_layout.c src/pe_reloc.c src/pw_guest_heap.c
    src/pe_export.c src/pe_tls.c src/pw_export.c src/pw_tls.c src/pw_sha256.c
    src/pw_wine_gate.c
    src/pw_guest_vm.c
    src/pw_guest_process.c
    src/pw_nt_handle.c
    src/pw_unixlib.c
    src/pw_wine_unixlib.c
    src/pw_wine_runner.c
    src/pw_wine_path.c
    src/pw_wine_handle.c
    src/pw_wine_file.c
    src/pw_wine_registry.c
    src/pw_wine_seed_services.c
    src/pw_wine_query.c
    src/pw_wine_section.c
    src/pw_wine_object.c
    src/pw_wine_thread.c
    src/pw_unix_call.c
    src/pw_compat32.c src/pw_gate.c src/pw_loader.c src/pw_map.c
    src/pw_module_name.c src/pw_result.c src/pw_segment.c src/pw_vm.c src/pw_ini.c
    src/pw_vm_posix.c src/pw_exec_probe.c src/pw_x86_block.c src/pw_x86_cache.c src/pw_x86_engine.c src/pw_x87.c src/pw_guest_call.c src/pw_import_bind.c src/pw_win32.c src/pw_user32.c src/pw_pad.c src/pw_gdi.c src/pw_present.c src/pw_crt_format.c src/pw_registry.c src/pw_registry_store.c src/pw_guest_fp.c src/pw_guest_args.c src/pe_resource.c src/pw_app_profile.c src/pw_prefix.c src/pw_runtime_supervisor.c
)
objects=()
for source in "${sources[@]}"; do
    object="$build/obj/${source//\//_}.o"
    "${cc[@]}" -std=c11 "${common[@]}" -c "$root/$source" -o "$object"
    objects+=("$object")
done
"${cc[@]}" -c "$root/src/pw_win64_call.S" -o "$build/obj/pw_win64_call.o"
objects+=("$build/obj/pw_win64_call.o")
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
echo "staged $(find "$dist/win" -type f | wc -l) image(s) under $dist/win"
