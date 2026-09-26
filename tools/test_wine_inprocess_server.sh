#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Host test of Wine's in-process server (wine/patches/0110). Applies the
# patch series to a private copy of the pinned Wine revision, configures it
# for the host, builds the server objects with WINE_INPROCESS_SERVER, links
# them into tests/wine_inprocess_server_check.c (main renamed away) and runs
# it with a scratch WINEPREFIX. Needs the host Wine build for its tools, as
# tools/build_wine_ps5.sh does; it is not part of `make test`.
#
# Usage: tools/test_wine_inprocess_server.sh [--work DIR]
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${PROSPERO_WINE_HOST_WORK:-$root/.deps/wine-host-inprocess}
source_dir=${PROSPERO_WINE_SOURCE:-$root/.deps/wine/source}
host_tools=${PROSPERO_WINE_BUILD:-$root/.deps/wine/build}
commit=$(sed -n 's/^WINE_COMMIT=//p' "$root/tools/build_wine_ps5.sh")

fail() { echo "test_wine_inprocess_server: $*" >&2; exit 1; }
[ "${1:-}" != --work ] || { work=$2; shift 2; }
[ $# -eq 0 ] || fail "unknown argument $1"
[ -x "$host_tools/tools/winebuild/winebuild" ] || fail "no host Wine tools at $host_tools"
[ "$(git -C "$source_dir" rev-parse HEAD 2>/dev/null)" = "$commit" ] ||
    fail "$source_dir is not the pinned Wine revision $commit"

mkdir -p "$work"
tree=$work/source
[ -d "$tree/.git" ] || git clone -q --shared --no-checkout "$source_dir" "$tree"
git -C "$tree" checkout -q --force "$commit"
git -C "$tree" clean -q -fdx
for patch in $("$root/tools/build_wine_ps5.sh" --check-patches); do
    git -C "$tree" apply --index "$root/wine/patches/$patch" || fail "patch does not apply: $patch"
done

build=$work/build
if [ ! -f "$build/Makefile" ]; then
    mkdir -p "$build"
    (cd "$build" && "$tree/configure" --enable-archs=x86_64 --disable-tests --without-x \
        --without-freetype --without-fontconfig --without-gnutls --without-vulkan \
        --without-wayland --without-opengl --with-wine-tools="$host_tools" \
        > "$work/configure.log" 2>&1) || fail "configure failed; see $work/configure.log"
fi
# CFLAGS is not a make dependency: rebuild the server objects every run.
rm -f "$build"/server/*.o
make -C "$build" -j"$(getconf _NPROCESSORS_ONLN)" CFLAGS="-g -O1 -DWINE_INPROCESS_SERVER" \
    server/wineserver > "$work/make.log" 2>&1 || fail "server build failed; see $work/make.log"

objcopy --redefine-sym main=wineserver_main "$build/server/main.o" "$work/server_main.o"
objects=$(ls "$build"/server/*.o | grep -v '/main\.o$')
# shellcheck disable=SC2086
cc -g -O1 -Wall -Wextra -Werror -std=c11 "$root/tests/wine_inprocess_server_check.c" \
    "$work/server_main.o" $objects -lm -pthread -o "$work/wine_inprocess_server_check"

version=$(sed -n 's/^#define SERVER_PROTOCOL_VERSION \([0-9]*\)$/\1/p' "$tree/include/wine/server_protocol.h")
prefix=$(mktemp -d "$work/prefix.XXXXXX")
WINEPREFIX=$prefix timeout 60 "$work/wine_inprocess_server_check" "$tree/nls" "$version"
rm -rf "$prefix"
