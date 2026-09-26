#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Cross-build Wine's Unix side (ntdll.so, win32u.so, wineserver) for the PS5.
#
# The pinned Wine revision is copied into a private work tree, the patch
# series in wine/patches is applied in numeric order, and the copy is
# configured for FreeBSD with the PS5 payload SDK: the PS5 compiler defines
# __FreeBSD__ and ships FreeBSD headers, so Wine takes its FreeBSD paths
# (kqueue, sysctl) and __PROSPERO__ selects the PS5 patches. PE modules are
# not built here; they come from the host build (tools/build_wine_runtime.sh),
# whose tools/ directory also supplies winebuild and widl.
#
# Patch numbers are owned by range: 0100-0499 services, loader and
# presentation; 0500-0899 execution core (signals, TEB, virtual memory).
#
# The link uses the SDK's stub libraries and reports, rather than fails on,
# unresolved symbols, so every run measures exactly what the console's libc
# and kernel do not provide. The result is written to <work>/report.json.
#
# Usage:
#   tools/build_wine_ps5.sh [--check-patches] [--patches DIR] [--work DIR]
#       [--source DIR] [--host-tools DIR] [--sdk DIR] [--jobs N]
set -eu

WINE_COMMIT=490f6d5dcbb2a5047345b8af88d114bbcaad69a8
TARGETS="dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver"
# Everything optional is off: the console has none of these libraries, and a
# configure-time probe against the payload SDK must not pick up host headers.
CONFIGURE_ARGS="--host=x86_64-unknown-freebsd11 --build=x86_64-pc-linux-gnu
 --enable-archs=x86_64 --disable-tests --without-x --without-freetype
 --without-fontconfig --without-gnutls --without-alsa --without-pulse
 --without-dbus --without-gstreamer --without-sdl --without-udev --without-usb
 --without-v4l2 --without-vulkan --without-wayland --without-opengl --without-oss
 --without-pcap --without-pcsclite --without-sane --without-krb5 --without-gphoto
 --without-netapi --without-inotify --without-capi --without-cups --without-gssapi
 --without-ffmpeg"

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
patches=$root/wine/patches
work=${PROSPERO_WINE_PS5_WORK:-$root/.deps/wine-ps5}
source_dir=${PROSPERO_WINE_SOURCE:-$root/.deps/wine/source}
host_tools=${PROSPERO_WINE_BUILD:-$root/.deps/wine/build}
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk}
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
check_only=0

fail() { echo "build_wine_ps5: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case $1 in
    --check-patches) check_only=1 ;;
    --patches) patches=$2; shift ;;
    --work) work=$2; shift ;;
    --source) source_dir=$2; shift ;;
    --host-tools) host_tools=$2; shift ;;
    --sdk) sdk=$2; shift ;;
    --jobs) jobs=$2; shift ;;
    *) fail "unknown argument $1" ;;
    esac
    shift
done

# The series: NNNN-lower-case-name.patch, unique numbers below 0900, each a
# mail-formatted patch with a subject. Printed in the order it is applied.
series() {
    [ -d "$patches" ] || fail "no patch directory $patches"
    last=-1
    for patch in $(cd "$patches" && ls | LC_ALL=C sort); do
        case $patch in
        [0-9][0-9][0-9][0-9]-*.patch) ;;
        *) fail "unexpected file in the patch series: $patch" ;;
        esac
        printf '%s\n' "$patch" | grep -Eq '^[0-9]{4}-[a-z0-9][a-z0-9-]*\.patch$' ||
            fail "patch name must be NNNN-lower-case-words.patch: $patch"
        number=$(printf '%s' "$patch" | cut -c1-4 | sed 's/^0*//')
        number=${number:-0}
        [ "$number" -ge 100 ] && [ "$number" -le 899 ] ||
            fail "patch number outside 0100-0899: $patch"
        [ "$number" -ne "$last" ] || fail "duplicate patch number: $patch"
        last=$number
        grep -q '^Subject: ' "$patches/$patch" || fail "patch has no Subject: $patch"
        printf '%s\n' "$patch"
    done
}

ordered=$(series)
if [ "$check_only" -eq 1 ]; then
    printf '%s\n' "$ordered"
    exit 0
fi

[ -x "$sdk/bin/prospero-clang" ] || fail "no PS5 payload SDK at $sdk"
[ -x "$host_tools/tools/winebuild/winebuild" ] ||
    fail "no host Wine tools at $host_tools (run tools/build_wine_runtime.sh)"
[ "$(git -C "$source_dir" rev-parse HEAD 2>/dev/null)" = "$WINE_COMMIT" ] ||
    fail "$source_dir is not the pinned Wine revision $WINE_COMMIT"

# A private copy keeps the shared checkout pristine for the host build.
mkdir -p "$work"
tree=$work/source
if [ ! -d "$tree/.git" ]; then
    git clone -q --shared --no-checkout "$source_dir" "$tree"
fi
git -C "$tree" checkout -q --force "$WINE_COMMIT"
git -C "$tree" clean -q -fdx
for patch in $ordered; do
    git -C "$tree" apply --index "$patches/$patch" || fail "patch does not apply: $patch"
    echo "applied $patch"
done

# Reconfigure whenever the patches or the arguments change.
stamp=$(
    { printf '%s\n' "$WINE_COMMIT" "$CONFIGURE_ARGS" "$sdk"
      for patch in $ordered; do cat "$patches/$patch"; done; } | sha256sum | cut -c1-64)
build=$work/build
if [ ! -f "$build/Makefile" ] || [ "$(cat "$build/.prospero-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$build"
    mkdir -p "$build"
    # shellcheck disable=SC2086
    (cd "$build" && "$tree/configure" $CONFIGURE_ARGS CC="$sdk/bin/prospero-clang" \
        --with-wine-tools="$host_tools" > "$work/configure.log" 2>&1) ||
        fail "configure failed; see $work/configure.log"
    echo "$stamp" > "$build/.prospero-stamp"
fi

# The SDK's libc carries the math functions, but win32u links -lm by name.
mkdir -p "$work/ps5lib"
[ -f "$work/ps5lib/libm.a" ] || "$sdk/bin/llvm-ar" rcs "$work/ps5lib/libm.a"
# The heap (wine/ps5) serves the malloc family. ntdll.so links and exports
# it, so win32u.so binds to the same instance through its ntdll.so import;
# the in-process wineserver has its own copy.
mkdir -p "$work/heap"
heap=""
for unit in pw_wine_heap pw_wine_heap_libc; do
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
        -c "$root/wine/ps5/$unit.c" -o "$work/heap/$unit.o" || fail "cannot compile $unit.c"
    heap="$heap $work/heap/$unit.o"
done
base="-L$work/ps5lib -lunwind -Wl,--warn-unresolved-symbols"
status=0
: > "$work/make.log"
# LDFLAGS is not a make dependency, so relink the three targets every run.
(cd "$build" && rm -f $TARGETS)
for step in "dlls/ntdll/ntdll.so|$heap" "dlls/win32u/win32u.so|" "server/wineserver|$heap"; do
    target=${step%%|*}; objects=${step#*|}
    make -C "$build" -k -j"$jobs" LDFLAGS="$objects $base" "$target" \
        >> "$work/make.log" 2>&1 || status=$?
done

python3 - "$build" "$work/make.log" "$work/report.json" "$sdk" "$WINE_COMMIT" $ordered <<'PY'
import hashlib, json, re, shutil, subprocess, sys
from pathlib import Path
build, log, report, sdk, commit = sys.argv[1:6]
patches = sys.argv[6:]
text = Path(log).read_text(errors="replace")
owners = {"dlls/ntdll/": "dlls/ntdll/ntdll.so", "dlls/win32u/": "dlls/win32u/win32u.so",
          "server/": "server/wineserver"}
unresolved = {target: set() for target in owners.values()}
# lld prints each unresolved symbol, then ">>> referenced by" lines whose
# continuation names the object ("dir/file.o:(function)"); the object's
# directory names the target whose link reported it.
symbol = None
for line in text.splitlines():
    match = re.search(r"undefined symbol: (\S+)", line)
    if match:
        symbol = match.group(1)
        continue
    match = re.search(r">>>\s+(\S+\.o):", line)
    if match and symbol:
        for prefix, target in owners.items():
            if match.group(1).startswith(prefix):
                unresolved[target].add(symbol)
readelf = shutil.which("llvm-readelf-18") or shutil.which("llvm-readelf") or f"{sdk}/bin/llvm-readelf"
result = {"schema": 1, "wine_commit": commit, "patches": patches, "targets": {}}
for target in owners.values():
    path = Path(build) / target
    entry = {"built": path.is_file(), "unresolved": sorted(unresolved[target])}
    if path.is_file():
        data = path.read_bytes()
        entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        dynamic = subprocess.run([readelf, "-d", str(path)], capture_output=True, text=True).stdout
        entry["needed"] = re.findall(r"Shared library: \[([^\]]+)\]", dynamic)
        symbols = subprocess.run([f"{sdk}/bin/llvm-nm", "-D", str(path)],
                                 capture_output=True, text=True).stdout.split("\n")
        kinds = {line.split()[-2] for line in symbols
                 if line.split() and line.split()[-1] == "malloc" and len(line.split()) >= 2}
        entry["malloc"] = "defines" if kinds & {"T", "t"} else "imports" if "U" in kinds else "absent"
    result["targets"][target] = entry
result["errors"] = sorted(set(re.findall(r"error: (.+)", text)))
Path(report).write_text(json.dumps(result, indent=2) + "\n")
for target, entry in result["targets"].items():
    print(f"{target}: built={entry['built']} malloc={entry.get('malloc', '-')} "
          f"unresolved={','.join(entry['unresolved']) or 'none'}")
PY
[ "$status" -eq 0 ] || fail "build failed; see $work/make.log"
echo "report: $work/report.json"
