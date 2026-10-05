#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The app folder a player uploads to /data/homebrew: the title from
# tools/build_native.sh with its Wine runtime beside it, under win/wine.
#
#   PPSA99995/
#     eboot.bin, sce_sys/, sce_module/          the title (no dev.conf)
#     win/wine/lib/wine/i386-windows/           Wine's PE modules
#     win/wine/lib/wine/x86_64-windows/
#     win/wine/lib/wine/x86_64-unix/*.prx       Wine's PS5 side
#     win/wine/share/wine/nls/, fonts/
#     LICENSE, THIRD_PARTY.md, LICENSES/       the licences of everything above
#     SOURCES.txt                               the source revision of each part
#
# Where each part comes from:
#   --title DIR      tools/build_native.sh's dist/PPSA99995. Its dev.conf,
#                    which names the builder's PC, is left out.
#   --wine-ps5 DIR   tools/build_wine_ps5.sh's work directory
#                    (.deps/wine-ps5): prx/sce_module/*.prx, prx/fonts/,
#                    the PE modules its patches change (pe/), report.json
#                    for the revisions behind them, and the Wine and
#                    FreeType sources it built, for their licence files.
#   --host-wine DIR  tools/build_host_wine.sh's installation (its usr/): the
#                    same revision and patches, whose PE modules and NLS
#                    files the console runs. Import libraries and the
#                    drivers that need the PC's own libraries are left out.
#   --cpu-dll FILE   tools/build_wowprospero.sh's
#                    x86_64-windows/wowprospero.dll.
#   --lapy-release FILE  the release.json tools/build_native.sh saved for the
#                    lapy.elf it fetched (build/native/lapy-helper-release.json).
#   --out DIR        where PPSA99995/ is written (replaced).
#   --zip            also write DIR/PPSA99995.zip.
#
# A game's prefix gets its own copy of wowprospero.dll from
# tools/pw_prefix.py push --cpu-dll.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
title= wine_ps5= host_wine= cpu_dll= lapy_release= out= zip=0
fail() { echo "package_release: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case $1 in
    --title) title=$2; shift 2 ;;
    --wine-ps5) wine_ps5=$2; shift 2 ;;
    --host-wine) host_wine=$2; shift 2 ;;
    --cpu-dll) cpu_dll=$2; shift 2 ;;
    --lapy-release) lapy_release=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --zip) zip=1; shift ;;
    *) fail "unknown argument $1" ;;
    esac
done
[ -f "$title/eboot.bin" ] && [ -d "$title/sce_sys" ] || fail "--title: no eboot.bin and sce_sys/ in '$title'"
[ -f "$title/lapy.elf" ] || fail "--title: no lapy.elf in '$title'"
ls "$wine_ps5"/prx/sce_module/ntdll.prx >/dev/null 2>&1 || fail "--wine-ps5: no prx/sce_module/ntdll.prx in '$wine_ps5'"
[ -d "$wine_ps5/prx/fonts" ] || fail "--wine-ps5: no prx/fonts in '$wine_ps5'"
[ -d "$host_wine/lib/wine/i386-windows" ] && [ -d "$host_wine/lib/wine/x86_64-windows" ] &&
    [ -d "$host_wine/share/wine/nls" ] || fail "--host-wine: '$host_wine' is not a WoW64 Wine installation (usr/)"
[ -f "$cpu_dll" ] || fail "--cpu-dll: no file '$cpu_dll'"
[ -f "$lapy_release" ] || fail "--lapy-release: no file '$lapy_release'"
[ -f "$wine_ps5/report.json" ] || fail "--wine-ps5: no report.json in '$wine_ps5'"
for file in LICENSE COPYING.LIB AUTHORS NOTICES.md; do
    [ -f "$wine_ps5/source/$file" ] || fail "--wine-ps5: no Wine source/$file in '$wine_ps5'"
done
[ -f "$wine_ps5/freetype/src/docs/FTL.TXT" ] ||
    fail "--wine-ps5: no FreeType source (freetype/src/docs/FTL.TXT) in '$wine_ps5'"
[ -n "$out" ] || fail "--out DIR is required"

app=$out/PPSA99995
rm -rf "$app"
mkdir -p "$app"
# The title, without the builder's log destination.
( cd "$title" && find . -type f ! -name dev.conf ) | while read -r file; do
    mkdir -p "$app/$(dirname "$file")"
    cp "$title/$file" "$app/$file"
done

lib=$app/win/wine/lib/wine
share=$app/win/wine/share/wine
mkdir -p "$lib/x86_64-unix" "$share/nls" "$share/fonts"
for arch in i386-windows x86_64-windows; do
    mkdir -p "$lib/$arch"
    for file in "$host_wine/lib/wine/$arch"/*; do
        name=${file##*/}
        case $name in
        # Import libraries, and what needs the PC's own libraries: X11,
        # GStreamer, pcap, SANE and libgphoto2.
        *.a|winex11.drv|winegstreamer.dll|wpcap.dll|sane.ds|gphoto2.ds) continue ;;
        esac
        cp "$file" "$lib/$arch/$name"
    done
    # The PE modules the PS5 patches change (the xinput DLLs).
    if [ -d "$wine_ps5/pe/$arch" ]; then cp "$wine_ps5/pe/$arch"/*.dll "$lib/$arch/"; fi
done
cp "$cpu_dll" "$lib/x86_64-windows/wowprospero.dll"
cp "$wine_ps5"/prx/sce_module/*.prx "$lib/x86_64-unix/"
cp "$host_wine"/share/wine/nls/* "$share/nls/"
cp "$wine_ps5"/prx/fonts/* "$share/fonts/"
# The PS5 kernel refuses to exec an eboot without execute permission and its
# loader refuses a PRX without it, so every executable module is 0755.
chmod 755 "$app/eboot.bin"
find "$app" -type f -name '*.prx' -exec chmod 755 {} +

# The licences (THIRD_PARTY.md says which covers what): the project's own
# texts, then Wine's and FreeType's from the sources that were built.
cp "$root/LICENSE" "$root/THIRD_PARTY.md" "$app/"
cp -R "$root/LICENSES" "$app/LICENSES"
mkdir -p "$app/LICENSES/wine/libs" "$app/LICENSES/freetype"
for file in LICENSE COPYING.LIB AUTHORS NOTICES.md; do cp "$wine_ps5/source/$file" "$app/LICENSES/wine/"; done
for file in "$wine_ps5"/source/libs/*/LICENSE* "$wine_ps5"/source/libs/*/COPYING* \
    "$wine_ps5"/source/libs/*/COPYRIGHT*; do
    [ -f "$file" ] || continue
    library=$(basename "$(dirname "$file")")
    mkdir -p "$app/LICENSES/wine/libs/$library"
    cp "$file" "$app/LICENSES/wine/libs/$library/"
done
cp "$wine_ps5/freetype/src/LICENSE.TXT" "$wine_ps5/freetype/src/docs/FTL.TXT" "$app/LICENSES/freetype/"

# SOURCES.txt: the revision of each part, from the build's own records.
python3 - "$root" "$wine_ps5/report.json" "$lapy_release" "$app" <<'PY' || exit 2
import json, re, subprocess, sys
from pathlib import Path
root, report, lapy, app = Path(sys.argv[1]), json.loads(Path(sys.argv[2]).read_text()), \
    json.loads(Path(sys.argv[3]).read_text()), Path(sys.argv[4])
def pin(script, name):
    return re.search(rf"^{name}=(\S+)$", (root / "tools" / script).read_text(), re.M).group(1)
def fail(message):
    sys.exit(f"package_release: {message}")
git = lambda *args: subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True).stdout.strip()
sources = report.get("sources") or {}
if not report.get("wine_commit"):
    fail("--wine-ps5: report.json names no Wine commit")
if not lapy.get("tag_name"):
    fail("--lapy-release: no tag_name")
vulkan = (app / "win/wine/lib/wine/x86_64-unix/libvulkan.prx").exists()
if vulkan and not sources.get("ps5_mesa"):
    fail("libvulkan.prx is not a RADV build in report.json; THIRD_PARTY.md covers RADV only")
commit = (git("rev-parse", "HEAD") or "not recorded") + \
    (" (with uncommitted changes)" if git("status", "--porcelain") else "")
lines = [
    "Source for this package. THIRD_PARTY.md says what each part is and its licence.",
    "",
    f"prospero-win  https://github.com/mpereiraesaa/prospero-win  commit {commit}",
    f"Wine  https://gitlab.winehq.org/wine/wine  commit {report['wine_commit']},",
    f"  with prospero-win's wine/patches ({len(report.get('patches', []))} patches)",
    "PS5 Native App Boilerplate  https://github.com/mpereiraesaa/ps5-native-app-boilerplate",
    f"  title: commit {pin('build_native.sh', 'pin')}",
    f"  PS5 module tools: commit {sources.get('prx_foundation') or 'not recorded'}",
    "PS5 payload SDK  https://github.com/ps5-payload-dev/sdk  the release the boilerplate pins",
    f"FreeType {pin('build_wine_ps5.sh', 'FREETYPE_VERSION')}  "
    f"{pin('build_wine_ps5.sh', 'FREETYPE_URL').replace('$FREETYPE_VERSION', pin('build_wine_ps5.sh', 'FREETYPE_VERSION'))}",
    f"  SHA-256 {pin('build_wine_ps5.sh', 'FREETYPE_SHA256')}",
    f"Lapy JB Daemon (lapy.elf)  {lapy.get('release_url') or 'https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon'}"
    f"  release {lapy['tag_name']}",
]
if vulkan:
    lines += [f"RADV (libvulkan.prx)  https://github.com/mpereiraesaa/PS5_Mesa  commit {sources['ps5_mesa']}",
              f"  linked by https://github.com/mpereiraesaa/PS5_Vulkan  commit {sources.get('ps5_vulkan') or 'not recorded'}"]
if sources.get("ps5_opengl_sdk"):
    lines += ["OpenGL (in win32u.prx)  https://github.com/mpereiraesaa/ps5-opengl",
              f"  SDK manifest SHA-256 {sources['ps5_opengl_sdk']}"]
else:
    lines += ["OpenGL  not included"]
(app / "SOURCES.txt").write_text("\n".join(lines) + "\n")
PY

files=$(find "$app" -type f | wc -l)
size=$(du -sh "$app" | cut -f1)
echo "package_release: $app: $files files, $size"
if [ "$zip" = 1 ]; then
    rm -f "$out/PPSA99995.zip"
    ( cd "$out" && zip -qr PPSA99995.zip PPSA99995 )
    echo "package_release: $out/PPSA99995.zip"
fi
