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
# Two links follow. The ELF link builds Wine's own targets against the
# payload SDK and reports, rather than fails on, unresolved symbols. The PRX
# link is what the console loads: ntdll, win32u and the in-process
# wineserver (patch 0110) linked against the
# title's stub libraries with the PS5 shims in wine/ps5 and a generated
# export descriptor, then converted and signed with ps5-native-tool into
# <work>/prx/sce_module. Both are measured in <work>/report.json. Module
# conversion needs a foundation with module export publishing (its branch
# exp/prx-module, MODULE_EXPORTS_COMMIT); the title's pinned foundation
# predates it, so --prx-foundation names that checkout, and without it the
# PRX link is skipped and says why.
#
# Usage:
#   tools/build_wine_ps5.sh [--check-patches] [--patches DIR] [--work DIR]
#       [--source DIR] [--host-tools DIR] [--foundation DIR] [--sdk DIR]
#       [--prx-foundation DIR] [--jobs N]
set -eu

WINE_COMMIT=490f6d5dcbb2a5047345b8af88d114bbcaad69a8
MODULE_EXPORTS_COMMIT=5bd0887e983abbf2f8a2eb762da8d4501b543179
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
foundation=${PS5_NATIVE_FOUNDATION:-$root/.deps/ps5-native-app-boilerplate}
sdk=${PS5_PAYLOAD_SDK:-}
prx_foundation=${PS5_PRX_FOUNDATION:-}
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
    --foundation) foundation=$2; shift ;;
    --sdk) sdk=$2; shift ;;
    --prx-foundation) prx_foundation=$2; shift ;;
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

sdk=${sdk:-$foundation/.deps/native/ps5-payload-sdk}
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

# The SDK's libc carries the math functions, but win32u links -lm by name;
# libunwind.a names pthread as a dependent library, which the title's
# libkernel stubs provide.
mkdir -p "$work/ps5lib"
for library in libm libpthread; do
    [ -f "$work/ps5lib/$library.a" ] || "$sdk/bin/llvm-ar" rcs "$work/ps5lib/$library.a"
done
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

# The PRX link. Each module takes the objects of its ELF link (read back
# from make.log), the shims, and a descriptor naming what its loader
# resolves: the title calls into ntdll, and ntdll's dlsym into win32u.
prx_foundation=${prx_foundation:-$foundation}
tool=$prx_foundation/build/host/ps5-native-tool
pie=$prx_foundation/tooling/native/ps5-pie.ld
prx=$work/prx
rm -rf "$prx"
mkdir -p "$prx/obj" "$prx/sce_module"
if [ ! -x "$tool" ] || [ ! -f "$pie" ]; then
    prx_status="skipped: no built ps5-native-tool in $prx_foundation"
elif ! git -C "$prx_foundation" merge-base --is-ancestor "$MODULE_EXPORTS_COMMIT" HEAD 2>/dev/null; then
    prx_status="skipped: $prx_foundation lacks module exports ($MODULE_EXPORTS_COMMIT)"
else
    prx_status=0
fi
link_objects() {
    python3 - "$work/make.log" "$1" <<'PY'
import shlex, sys
text = open(sys.argv[1], errors="replace").read().replace("\\\n", " ")
lines = [line for line in text.splitlines() if f"-o {sys.argv[2]} " in line]
if lines:
    print(" ".join(arg for arg in shlex.split(lines[-1])
                   if arg.endswith(".o") and not arg.startswith("/")))
PY
}
# link_prx NAME TARGET "EXTRA OBJECTS" [IMPORTED MODULE]
link_prx() {
    name=$1; objects=$(link_objects "$2")
    log=$prx/$name.link.log
    [ -n "$objects" ] || { echo "no ELF link of $2 in make.log" > "$log"; prx_status=1; return; }
    # shellcheck disable=SC2086
    (cd "$build" && "$sdk/bin/prospero-lld" --shared -Bsymbolic -T "$pie" \
        -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr -soname "$name.prx" -z defs \
        --warn-unresolved-symbols -L"$work/ps5lib" -o "$prx/$name.shared.elf" $objects $3 ${4:-} \
        "$sdk/target/lib/libunwind.a" --as-needed "$sdk"/target/lib/*.so) > "$log" 2>&1 &&
    "$tool" link --module --in "$prx/$name.shared.elf" --out "$prx/$name.elf" \
        --stub-dir "$sdk/target/lib" ${4:+--stub "$4"} --module-sdk 0x02000009 \
        --companion-sdk 0x08050001 --file-name "$name.prx" >> "$log" 2>&1 &&
    "$tool" self --sign --in "$prx/$name.elf" --out "$prx/sce_module/$name.prx" >> "$log" 2>&1 ||
        prx_status=1
}
if [ "$prx_status" = 0 ]; then
    for unit in pw_wine_prx pw_wine_dl pw_wine_dl_libc pw_wine_compat pw_wine_compat_libc \
            pw_wine_threads pw_wine_threads_libc pw_wine_sink pw_wine_cwd pw_wine_cwd_libc; do
        "$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC \
            -c "$root/wine/ps5/$unit.c" -o "$prx/obj/$unit.o" || fail "cannot compile $unit.c"
    done
    # ntdll: the loader, compat, and the title's present and input sink; the
    # thread registry belongs to the server alone.
    shims=""
    for unit in pw_wine_prx pw_wine_dl pw_wine_dl_libc pw_wine_compat pw_wine_compat_libc \
            pw_wine_sink pw_wine_cwd pw_wine_cwd_libc; do
        shims="$shims $prx/obj/$unit.o"
    done
    # A title cannot chdir: ntdll and wineserver each keep their own working
    # directory, and their path calls go through it (wine/ps5/pw_wine_cwd.h).
    cwd_wraps=""
    for name in $(grep -o '__wrap_[a-z_]*' "$root/wine/ps5/pw_wine_cwd_libc.c" | sed 's/^__wrap_//' |
            LC_ALL=C sort -u); do
        cwd_wraps="$cwd_wraps --wrap=$name"
    done
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/ntdll_desc.c" \
        __wine_main pw_wine_dl_adopt dlopen dlsym dlerror \
        pw_wine_heap_stats pw_wine_heap_malloc pw_wine_heap_free \
        pw_wine_set_present_sink pw_wine_post_input pw_wine_sink_stats \
        pw_wine_present pw_wine_next_input pw_wine_input_fd \
        __wine_virtual_stats __wine_ps5_set_output_sink
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/win32u_desc.c" __wine_unix_lib_init
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wineserver_desc.c" \
        pw_wineserver_connect pw_wine_thread_register
    for unit in ntdll_desc win32u_desc wineserver_desc; do
        "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$root/wine/ps5" \
            -c "$prx/obj/$unit.c" -o "$prx/obj/$unit.o" || fail "cannot compile $unit.c"
    done
    link_prx ntdll dlls/ntdll/ntdll.so "$heap $shims $prx/obj/ntdll_desc.o $cwd_wraps"
    link_prx win32u dlls/win32u/win32u.so "$prx/obj/win32u_desc.o" "$prx/ntdll.shared.elf"
    # ntdll loads it with its own dlopen; it has its own heap, needs no
    # dlfcn of its own, and signals threads through the registry ntdll fills.
    link_prx wineserver server/wineserver "$heap $prx/obj/pw_wine_compat.o \
        $prx/obj/pw_wine_compat_libc.o $prx/obj/pw_wine_threads.o \
        $prx/obj/pw_wine_threads_libc.o $prx/obj/wineserver_desc.o \
        $prx/obj/pw_wine_cwd.o $prx/obj/pw_wine_cwd_libc.o $cwd_wraps"
fi

if python3 - "$build" "$work/make.log" "$work/report.json" "$sdk" "$WINE_COMMIT" "$prx" "$prx_status" \
    $ordered <<'PY'
import hashlib, json, re, shutil, subprocess, sys
from pathlib import Path
build, log, report, sdk, commit, prx, prx_status = sys.argv[1:8]
patches = sys.argv[8:]
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
# Per PRX: what the stub link left unresolved, the tool's refusals, the
# modules it imports from, and its data imports, since a data import
# between application PRXs faults on the console (measured, FW 12.02).
result["prx"] = {"status": prx_status, "modules": {}}
def exports(library, directory=f"{sdk}/target/lib"):
    listing = subprocess.run([f"{sdk}/bin/llvm-nm", "-D", "--defined-only", f"{directory}/{library}"],
                             capture_output=True, text=True).stdout
    return {line.split()[-1].split("@")[0] for line in listing.splitlines() if line.split()}
title_exports = exports("libkernel.so") | exports("libSceLibcInternal.so")
objdump = shutil.which("llvm-objdump-18") or shutil.which("llvm-objdump") or f"{sdk}/bin/llvm-objdump"
SYSCALL_ALLOWED = {"__wine_syscall_dispatcher", "__wine_unix_call_dispatcher"}
ntdll_exports = exports("ntdll.shared.elf", prx) if (Path(prx) / "ntdll.shared.elf").is_file() else set()
for name in ("ntdll", "win32u", "wineserver") if not prx_status.startswith("skipped") else ():
    module = Path(prx) / "sce_module" / f"{name}.prx"
    link_log = Path(prx) / f"{name}.link.log"
    link_text = link_log.read_text(errors="replace") if link_log.is_file() else ""
    entry = {"built": module.is_file(),
             "unresolved": sorted(set(re.findall(r"undefined symbol: (\S+)", link_text))),
             "errors": sorted(set(re.findall(r"error: (.+)", link_text)))}
    shared = Path(prx) / f"{name}.shared.elf"
    if shared.is_file():
        dynamic = subprocess.run([readelf, "-d", str(shared)], capture_output=True, text=True).stdout
        entry["needed"] = re.findall(r"Shared library: \[([^\]]+)\]", dynamic)
        symbols = subprocess.run([readelf, "--dyn-syms", "-W", str(shared)],
                                 capture_output=True, text=True).stdout
        entry["data_imports"] = sorted({fields[7] for fields in (line.split() for line in symbols.splitlines())
                                        if len(fields) >= 8 and fields[3] == "OBJECT" and fields[6] == "UND"})
        # A game title does not get libkernel_sys: imports only it exports
        # stay 0 and a call to one jumps to 0 (measured, FW 12.02).
        imports = {fields[7].split("@")[0] for fields in (line.split() for line in symbols.splitlines())
                   if len(fields) >= 8 and fields[6] == "UND"}
        provided = title_exports | (ntdll_exports if name == "win32u" else set())
        entry["title_unbound"] = sorted((imports & exports("libkernel_sys.so")) - provided)
        # The kernel kills a title that executes a syscall instruction outside
        # libkernel (measured: SYSTEM_ILLEGAL_FUNCTION_CALL). Wine's dispatchers
        # keep one on the FS-base restore path, which patch 0500 never takes.
        listing = subprocess.run([objdump, "-d", "--no-show-raw-insn", str(shared)],
                                 capture_output=True, text=True).stdout
        function, found = None, set()
        for line in listing.splitlines():
            match = re.match(r"[0-9a-f]+ <(.+)>:$", line)
            if match:
                function = match.group(1)
            elif re.search(r"\tsyscall\b", line) and function not in SYSCALL_ALLOWED:
                found.add(function)
        entry["raw_syscalls"] = sorted(found)
    if module.is_file():
        data = module.read_bytes()
        entry.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    result["prx"]["modules"][name] = entry
Path(report).write_text(json.dumps(result, indent=2) + "\n")
for target, entry in result["targets"].items():
    print(f"{target}: built={entry['built']} malloc={entry.get('malloc', '-')} "
          f"unresolved={','.join(entry['unresolved']) or 'none'}")
if prx_status.startswith("skipped"):
    print(f"prx: {prx_status}")
for name, entry in result["prx"]["modules"].items():
    print(f"{name}.prx: built={entry['built']} needed={','.join(entry.get('needed', [])) or '-'} "
          f"unresolved={','.join(entry['unresolved']) or 'none'} "
          f"data_imports={','.join(entry.get('data_imports', [])) or 'none'}")
    for error in entry["errors"]:
        print(f"  error: {error}")
    if entry.get("title_unbound"):
        print(f"  unbound in a title (libkernel_sys only): {','.join(entry['title_unbound'])}")
    if entry.get("raw_syscalls"):
        print(f"  raw syscall instructions (fatal in a title): {','.join(entry['raw_syscalls'])}")
modules = result["prx"]["modules"].values()
sys.exit(3 if any(e.get("title_unbound") for e in modules) else
         4 if any(e.get("raw_syscalls") for e in modules) else 0)
PY
then title_status=0; else title_status=$?; fi
[ "$status" -eq 0 ] || fail "build failed; see $work/make.log"
[ "$title_status" -ne 3 ] || fail "PRX imports only libkernel_sys exports, which a title does not get"
# A warning until the working-directory shim stops issuing raw syscalls.
[ "$title_status" -ne 4 ] ||
    echo "build_wine_ps5: WARNING: PRX executes raw syscalls, which kill a title" >&2
[ "$prx_status" != 1 ] || fail "PRX link failed; see $prx/*.link.log"
echo "report: $work/report.json"
