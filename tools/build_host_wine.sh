#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The PC's Wine for installing games (tools/pw_install.py): the revision the
# title runs (WINE_COMMIT in build_wine_ps5.sh) with the same patch series,
# built for the desktop, so installers open their windows on the PC and the
# prefixes they write are ones the console reads as is. The PS5-only parts of
# the series are compiled only for the console (__PROSPERO__,
# WINE_PS5_USER_DRIVER).
#
# WoW64 (i386 and x86_64 PE modules, 64-bit Unix side), with X11, FreeType,
# fontconfig, Vulkan (DXVK on the PC), PulseAudio/ALSA and GnuTLS, whichever
# the PC has. It is installed under WORK/install and stripped, as Wine's
# distribution packages are: wineboot copies the PE modules into each prefix,
# about 300 MB stripped against 1.5 GB with debug information.
#
# Usage:
#   tools/build_host_wine.sh [--source DIR] [--work DIR] [--jobs N]
# prints the installed wine's path last. --source is a checkout of Wine that
# has WINE_COMMIT (default .deps/wine/source, as build_wine_ps5.sh).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir=${PROSPERO_WINE_SOURCE:-$root/.deps/wine/source}
work=${PROSPERO_HOST_WINE_WORK:-$root/.deps/wine-host}
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

fail() { echo "build_host_wine: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case $1 in
    --source) source_dir=$2; shift ;;
    --work) work=$2; shift ;;
    --jobs) jobs=$2; shift ;;
    *) fail "unknown argument $1" ;;
    esac
    shift
done

# One pin and one series: the console build's.
commit=$(sed -n 's/^WINE_COMMIT=//p' "$root/tools/build_wine_ps5.sh")
patches=$root/wine/patches
ordered=$(sh "$root/tools/build_wine_ps5.sh" --check-patches) || fail "the patch series is not well-formed"
[ -n "$commit" ] || fail "no WINE_COMMIT in build_wine_ps5.sh"
git -C "$source_dir" cat-file -e "$commit^{commit}" 2>/dev/null ||
    fail "$source_dir does not have the pinned Wine revision $commit"
for tool in i686-w64-mingw32-gcc x86_64-w64-mingw32-gcc i686-w64-mingw32-strip x86_64-w64-mingw32-strip; do
    command -v "$tool" >/dev/null || fail "$tool is required"
done

mkdir -p "$work"
tree=$work/source
[ -d "$tree/.git" ] || git clone -q --shared --no-checkout "$source_dir" "$tree"
git -C "$tree" checkout -q --force "$commit"
git -C "$tree" clean -q -fdx
for patch in $ordered; do
    git -C "$tree" apply --index "$patches/$patch" || fail "patch does not apply: $patch"
done

# The series includes sources that live in wine/ps5 (the Vulkan batching
# runtime, the shared clock and key-state ABIs); stage them as the console
# build does, or the tree does not configure.
python3 "$root/tools/stage_vk_batch.py" --source "$tree" --repo "$root" ||
    fail "cannot stage Vulkan command-stream runtime"
cp "$root/wine/ps5/time/pw_qpc_clock.h" "$tree/dlls/ntdll/pw_qpc_clock.h" ||
    fail "cannot stage shared-clock ABI"
cp "$root/wine/ps5/input/pw_key_shared.h" "$tree/dlls/win32u/pw_key_shared.h" ||
    fail "cannot stage shared-input ABI"

configure_args="--prefix=/usr --enable-archs=i386,x86_64 --disable-tests"
stamp=$({ printf '%s\n' "$commit" "$configure_args"
          for patch in $ordered; do cat "$patches/$patch"; done; } | sha256sum | cut -c1-64)
build=$work/build
if [ ! -f "$build/Makefile" ] || [ "$(cat "$build/.prospero-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$build"
    mkdir -p "$build"
    # shellcheck disable=SC2086
    (cd "$build" && "$tree/configure" $configure_args > "$work/configure.log" 2>&1) ||
        fail "configure failed; see $work/configure.log"
    echo "$stamp" > "$build/.prospero-stamp"
fi
# What the PC's Wine could not find is worth knowing: an installer without
# X11 or FreeType shows nothing, and without Vulkan DXVK cannot be tried.
grep -E "^configure: (WARNING|OpenGL|Vulkan)|won't be supported" "$work/configure.log" || true
make -C "$build" -j"$jobs" > "$work/make.log" 2>&1 || fail "the build failed; see $work/make.log"

install=$work/install
rm -rf "$install"
make -C "$build" install DESTDIR="$install" > "$work/install.log" 2>&1 ||
    fail "the install failed; see $work/install.log"
lib=$install/usr/lib/wine
find "$lib/i386-windows" -type f -exec i686-w64-mingw32-strip -s {} + 2>/dev/null || true
find "$lib/x86_64-windows" -type f -exec x86_64-w64-mingw32-strip -s {} + 2>/dev/null || true
find "$lib/x86_64-unix" "$install/usr/bin" -type f -exec strip --strip-unneeded {} + 2>/dev/null || true

wine=$install/usr/bin/wine
"$wine" --version > /dev/null || fail "the installed wine does not run"
echo "host wine: $("$wine" --version), $(du -sh "$install" | cut -f1) installed"
echo "$wine"
