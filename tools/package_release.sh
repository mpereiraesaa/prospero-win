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
#
# Where each part comes from:
#   --title DIR      tools/build_native.sh's dist/PPSA99995. Its dev.conf,
#                    which names the builder's PC, is left out.
#   --wine-ps5 DIR   tools/build_wine_ps5.sh's work directory
#                    (.deps/wine-ps5): prx/sce_module/*.prx, prx/fonts/,
#                    and the PE modules its patches change (pe/).
#   --host-wine DIR  tools/build_host_wine.sh's installation (its usr/): the
#                    same revision and patches, whose PE modules and NLS
#                    files the console runs. Import libraries and the
#                    drivers that need the PC's own libraries are left out.
#   --cpu-dll FILE   tools/build_wowprospero.sh's
#                    x86_64-windows/wowprospero.dll.
#   --out DIR        where PPSA99995/ is written (replaced).
#   --zip            also write DIR/PPSA99995.zip.
#
# A game's prefix gets its own copy of wowprospero.dll from
# tools/pw_prefix.py push --cpu-dll.
set -eu

title= wine_ps5= host_wine= cpu_dll= out= zip=0
fail() { echo "package_release: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case $1 in
    --title) title=$2; shift 2 ;;
    --wine-ps5) wine_ps5=$2; shift 2 ;;
    --host-wine) host_wine=$2; shift 2 ;;
    --cpu-dll) cpu_dll=$2; shift 2 ;;
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

files=$(find "$app" -type f | wc -l)
size=$(du -sh "$app" | cut -f1)
echo "package_release: $app: $files files, $size"
if [ "$zip" = 1 ]; then
    rm -f "$out/PPSA99995.zip"
    ( cd "$out" && zip -qr PPSA99995.zip PPSA99995 )
    echo "package_release: $out/PPSA99995.zip"
fi
