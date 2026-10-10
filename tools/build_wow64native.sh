#!/bin/sh
# Build the independent native WoW64 scaffold against an existing pinned Wine build.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir= build_dir= output_dir=
ps5_sdk= ps5_wine= ps5_ntdll= ps5_foundation= ps5_stubs=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source) source_dir=$2; shift 2 ;;
        --build) build_dir=$2; shift 2 ;;
        --output) output_dir=$2; shift 2 ;;
        --ps5-sdk) ps5_sdk=$2; shift 2 ;;
        --ps5-wine) ps5_wine=$2; shift 2 ;;
        --ps5-ntdll) ps5_ntdll=$2; shift 2 ;;
        --ps5-foundation) ps5_foundation=$2; shift 2 ;;
        --ps5-stubs) ps5_stubs=$2; shift 2 ;;
        *) echo "usage: $0 --source DIR --build DIR --output DIR" >&2; exit 2 ;;
    esac
done
fail() { echo "build_wow64native: $*" >&2; exit 1; }
[ -n "$source_dir" ] && [ -n "$build_dir" ] && [ -n "$output_dir" ] || fail "all paths are required"
[ "$(git -C "$source_dir" rev-parse HEAD)" = 490f6d5dcbb2a5047345b8af88d114bbcaad69a8 ] || fail "wrong Wine revision"
grep -q -- '--enable-archs=i386,x86_64' "$build_dir/config.log" || fail "not a WoW64 configuration"
[ -x "$build_dir/tools/winegcc/winegcc" ] && [ -f "$build_dir/dlls/ntdll/ntdll.so" ] || fail "prepare the existing Wine build first"
module=$root/wine/wow64native
mkdir -p "$output_dir/x86_64-windows" "$output_dir/x86_64-unix"
out=$(CDPATH= cd -- "$output_dir" && pwd)
gcc -m64 -O2 -g -fPIC -shared -Wl,-Bsymbolic -Wl,-soname,wow64native.so \
    -Wl,-z,defs -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT \
    -I"$module" -I"$build_dir/include" -I"$source_dir/include" \
    "$module/unix.c" "$build_dir/dlls/ntdll/ntdll.so" -o "$out/x86_64-unix/wow64native.so"
x86_64-w64-mingw32-gcc -c -o "$out/x86_64-windows/cpu.o" "$module/cpu.c" \
    -I"$module" -I"$build_dir/include" -I"$source_dir/include" -I"$source_dir/include/msvcrt" \
    -D_MSVCR_VER=0 -D__WINESRC__ -D__WINE_PE_BUILD -Wall -fno-strict-aliasing \
    -mcx16 -mcmodel=small -O2 -g -fno-builtin -fshort-wchar
(cd "$build_dir" && tools/winegcc/winegcc -o "$out/x86_64-windows/wow64native.dll" \
    --wine-objdir . --cc-cmd=x86_64-w64-mingw32-gcc -b x86_64-w64-mingw32 \
    -Wl,--wine-builtin -shared "$module/wow64native.spec" -nodefaultlibs \
    -Wl,--image-base,0x7a400000 "$out/x86_64-windows/cpu.o" \
    dlls/wow64/x86_64-windows/libwow64.a dlls/ntdll/x86_64-windows/libntdll.a \
    libs/winecrt0/x86_64-windows/libwinecrt0.a dlls/ntdll/x86_64-windows/libntdll.a \
    libs/compiler-rt/x86_64-windows/libcompiler-rt.a)
echo "build_wow64native: scaffold built; native execution requires a compatible ntdll signal bridge"

# Optional independent PRX build against an existing PS5 Wine configuration.
# Every input is explicit; never update the shared Wine stage or deploy a title.
if [ -n "$ps5_sdk$ps5_wine$ps5_ntdll$ps5_foundation$ps5_stubs" ]; then
    [ -n "$ps5_sdk" ] && [ -n "$ps5_wine" ] && [ -n "$ps5_ntdll" ] && \
        [ -n "$ps5_foundation" ] && [ -n "$ps5_stubs" ] || fail "all five PS5 inputs are required"
    [ "$(git -C "$ps5_wine/source" rev-parse HEAD)" = 490f6d5dcbb2a5047345b8af88d114bbcaad69a8 ] || fail "wrong PS5 Wine revision"
    [ -f "$ps5_wine/build/include/config.h" ] && [ -f "$ps5_ntdll" ] || fail "PS5 Wine inputs are missing"
    [ -x "$ps5_sdk/bin/prospero-clang" ] && [ -x "$ps5_foundation/build/host/ps5-native-tool" ] || fail "PS5 build tools are missing"
    [ -d "$ps5_stubs" ] || fail "PS5 import stubs are missing"
    [ ! -e "$ps5_stubs/libkernel_web.so" ] && [ ! -e "$ps5_stubs/libScePosixForWebKit.so" ] || fail "use title import stubs without WebKit libraries"
    prx=$out/ps5
    mkdir -p "$prx/obj"
    "$ps5_sdk/bin/prospero-clang" -std=gnu11 -O2 -fPIC -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT \
        -I"$module" -I"$ps5_wine/build/include" -I"$ps5_wine/source/include" \
        -c "$module/unix.c" -o "$prx/obj/unix.o"
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/descriptor.c" __wine_unix_call_funcs
    "$ps5_sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$root/wine/ps5" \
        -c "$prx/obj/descriptor.c" -o "$prx/obj/descriptor.o"
    (cd "$prx/obj" && "$ps5_sdk/bin/llvm-ar" x "$ps5_sdk/target/lib/libc.a" emutls.o)
    "$ps5_sdk/bin/prospero-lld" --shared -Bsymbolic \
        -T "$ps5_foundation/tooling/native/ps5-pie.ld" -T "$root/wine/ps5/prx_eh_frame.ld" \
        --eh-frame-hdr -soname wow64native.prx -z defs -o "$prx/wow64native.shared.elf" \
        "$prx/obj/unix.o" "$prx/obj/emutls.o" "$prx/obj/descriptor.o" \
        "$ps5_ntdll" "$ps5_sdk/target/lib/libunwind.a" --as-needed "$ps5_stubs"/*.so
    "$ps5_foundation/build/host/ps5-native-tool" link --module --in "$prx/wow64native.shared.elf" \
        --out "$prx/wow64native.elf" --stub-dir "$ps5_stubs" --stub "$ps5_ntdll" \
        --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name wow64native.prx
    "$ps5_foundation/build/host/ps5-native-tool" self --sign --in "$prx/wow64native.elf" \
        --out "$prx/wow64native.prx"
    echo "build_wow64native: PS5 PRX built; no deployment or backend selection performed"
fi
