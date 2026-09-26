#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build the prospero-win WoW64 CPU backend (wine/wowprospero) into a pinned
# Wine WoW64 build tree, so that tree runs i386 code through the IA-32 DBT.
#
# The Wine tree must be the pinned revision configured with
# --enable-archs=i386,x86_64 (tools/build_wine_runtime.sh creates one). This
# script completes the full host build when the loader is missing, because
# the backend runs under a complete Wine: ntdll.so, win32u.so and wineserver
# are Wine's own, not prospero-win reimplementations.
#
# Outputs, inside the Wine build tree (where a build-tree Wine looks for
# builtin modules and their Unix libraries):
#   dlls/wowprospero/x86_64-windows/wowprospero.dll
#   dlls/wowprospero/wowprospero.so
#
# Usage:
#   tools/build_wowprospero.sh [--build DIR] [--source DIR] [--jobs N]
set -eu

WINE_COMMIT=490f6d5dcbb2a5047345b8af88d114bbcaad69a8
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${PROSPERO_WINE_BUILD:-$root/.deps/wine/build}
source_dir=${PROSPERO_WINE_SOURCE:-$root/.deps/wine/source}
jobs=$(nproc 2>/dev/null || echo 4)

fail() { echo "build_wowprospero: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case $1 in
    --build) build_dir=$2; shift 2 ;;
    --source) source_dir=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    *) fail "unknown argument $1" ;;
    esac
done

[ -f "$build_dir/Makefile" ] || fail "no configured Wine build at $build_dir"
[ -d "$source_dir/dlls/wow64" ] || fail "no Wine source at $source_dir"
commit=$(git -C "$source_dir" rev-parse HEAD 2>/dev/null) || fail "cannot read Wine commit"
[ "$commit" = "$WINE_COMMIT" ] || fail "Wine source is $commit, expected $WINE_COMMIT"
grep -q -- "--enable-archs=i386,x86_64" "$build_dir/config.log" ||
    fail "Wine build is not a WoW64 (--enable-archs=i386,x86_64) configuration"

if [ ! -x "$build_dir/loader/wine" ] || [ ! -x "$build_dir/server/wineserver" ]; then
    echo "build_wowprospero: completing the host Wine build (make -j$jobs)"
    (cd "$build_dir" && make -j"$jobs") > "$build_dir/make-host.log" 2>&1 ||
        fail "host Wine build failed; see $build_dir/make-host.log"
fi

module=$root/wine/wowprospero
out=$build_dir/dlls/wowprospero
mkdir -p "$out/x86_64-windows"

dbt="src/pw_x86_engine.c src/pw_x86_block.c src/pw_x86_cache.c src/pw_x86_hostexec.c
     src/pw_x87.c src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c"
(cd "$root" && gcc -m64 -O2 -g -fPIC -shared -Wl,-Bsymbolic -Wl,-soname,wowprospero.so \
    -Wl,-z,defs -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT \
    -I"$module" -I"$build_dir/include" -I"$source_dir/include" -Isrc -Iinclude \
    "$module/unix.c" $dbt -o "$out/wowprospero.so") || fail "Unix library build failed"

x86_64-w64-mingw32-gcc -c -o "$out/x86_64-windows/cpu.o" "$module/cpu.c" \
    -I"$module" -I"$build_dir/include" -I"$source_dir/include" -I"$source_dir/include/msvcrt" \
    -D_MSVCR_VER=0 -D__WINESRC__ -D__WINE_PE_BUILD -Wall -fno-strict-aliasing \
    -mcx16 -mcmodel=small -O2 -g -fno-builtin -fshort-wchar || fail "PE object build failed"

# ntdll appears on both sides of winecrt0 because winecrt0's Unix-call and
# debug helpers import from it.
(cd "$build_dir" && tools/winegcc/winegcc -o "$out/x86_64-windows/wowprospero.dll" \
    --wine-objdir . --cc-cmd=x86_64-w64-mingw32-gcc -b x86_64-w64-mingw32 \
    -Wl,--wine-builtin -shared "$module/wowprospero.spec" -nodefaultlibs \
    "$out/x86_64-windows/cpu.o" dlls/wow64/x86_64-windows/libwow64.a \
    dlls/ntdll/x86_64-windows/libntdll.a libs/winecrt0/x86_64-windows/libwinecrt0.a \
    dlls/ntdll/x86_64-windows/libntdll.a libs/compiler-rt/x86_64-windows/libcompiler-rt.a) ||
    fail "PE link failed"

echo "build_wowprospero: $out/x86_64-windows/wowprospero.dll"
echo "build_wowprospero: $out/wowprospero.so"
