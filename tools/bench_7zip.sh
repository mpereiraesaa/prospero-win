#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# 7-Zip's built-in benchmark (`7za b`), the yardstick FEX and box64 publish,
# run on the host to compare the i386 DBT (wowprospero) with native
# execution of the same 32-bit binary. See docs/DBT_BENCHMARK.md.
#
# usage: tools/bench_7zip.sh --config CONFIG [options]
#   --config wow64cpu   i386 7za.exe under Wine WoW64, run natively by the CPU
#            wowprospero i386 7za.exe under Wine WoW64, run by our DBT
#            pe64       x64 7za.exe under Wine (same source and compiler, 64-bit)
#            linux      the host's own 7z/7zz (a different build: label it so)
#   --wine PATH         Wine's `wine` launcher (default: wine on PATH)
#   --prefix DIR        WINEPREFIX; wowprospero needs a prefix with
#                       wowprospero.dll installed (docs/WINE_INTEGRATION.md)
#   --runs N            repetitions (default 3)
#   --threads N         -mmt (default 1: the DBT translates per thread)
#   --dict N            -md, log2 of the dictionary (default 22)
#   --cpu LIST          taskset CPU list, for a stable core (default 0)
#   --modes DIGITS      PW_WOW_MODES for wowprospero (default: its own default)
#   --out DIR           where logs go (default build/bench-7zip)
#   --cache DIR         download cache (default ~/.cache/prospero-win/7zip)
# Prints one summary line per run and a final median; logs are kept in --out.
set -euo pipefail

SEVENZIP_VERSION=25.01
SEVENZIP_URL=https://www.7-zip.org/a/7z2501-extra.7z
SEVENZIP_SHA256=cd3cf38085c2cc6839cf72716dafb3175ae425f4fd34faafc6c0b64d618d307f

root=$(cd "$(dirname "$0")/.." && pwd)
config='' wine=wine prefix=${WINEPREFIX:-} runs=3 threads=1 dict=22 cpu=0 modes=''
out=$root/build/bench-7zip cache=${XDG_CACHE_HOME:-$HOME/.cache}/prospero-win/7zip
while (($#)); do
    case $1 in
        --config) config=$2 ;;
        --wine) wine=$2 ;;
        --prefix) prefix=$2 ;;
        --runs) runs=$2 ;;
        --threads) threads=$2 ;;
        --dict) dict=$2 ;;
        --cpu) cpu=$2 ;;
        --modes) modes=$2 ;;
        --out) out=$2 ;;
        --cache) cache=$2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift 2
done
case $config in wow64cpu|wowprospero|pe64|linux) ;; *) echo "--config is required" >&2; exit 2 ;; esac

# The pinned official package holds 7za.exe (i386) and x64/7za.exe. It is
# never committed: 7-Zip is LGPL with the unRAR restriction (its License.txt
# is extracted beside it).
fetch() {
    local archive=$cache/7z${SEVENZIP_VERSION/./}-extra.7z
    mkdir -p "$cache"
    if [[ ! -f $archive ]]; then
        curl -sSfL -o "$archive.part" "$SEVENZIP_URL"
        mv "$archive.part" "$archive"
    fi
    echo "$SEVENZIP_SHA256  $archive" | sha256sum -c --quiet - || {
        echo "7-Zip package checksum mismatch: $archive" >&2; exit 1; }
    if [[ ! -f $cache/extra/7za.exe ]]; then
        command -v 7z >/dev/null || { echo "extracting needs the host's 7z (p7zip)" >&2; exit 1; }
        7z x -y -o"$cache/extra" "$archive" 7za.exe x64/7za.exe License.txt >/dev/null
    fi
}

# HKLM\Software\Microsoft\Wow64\x86 names WoW64's i386 CPU backend.
select_cpu() {
    WINEPREFIX=$prefix WINEDEBUG=-all "$wine" reg add 'HKLM\Software\Microsoft\Wow64\x86' \
        /ve /d "$1.dll" /f >/dev/null 2>&1
}

command=()
case $config in
    linux)
        seven=$(command -v 7zz || command -v 7z) || { echo "no host 7z/7zz" >&2; exit 1; }
        command=("$seven") ;;
    *)
        [[ -n $prefix ]] || { echo "--prefix is required for $config" >&2; exit 1; }
        fetch
        exe=$cache/extra/7za.exe
        [[ $config == pe64 ]] && exe=$cache/extra/x64/7za.exe
        [[ $config == pe64 ]] || select_cpu "$config"
        command=("$wine" "$exe")
        export WINEPREFIX=$prefix WINEDEBUG=${WINEDEBUG:--all}
        [[ -n $modes ]] && export PW_WOW_MODES=$modes ;;
esac

mkdir -p "$out"
label=$config${modes:+-$modes}-mmt$threads-md$dict
logs=()
for ((i = 1; i <= runs; i++)); do
    log=$out/$label-$i.log
    taskset -c "$cpu" "${command[@]}" b "-mmt$threads" "-md$dict" >"$log" 2>&1 || {
        echo "run $i failed; see $log" >&2; exit 1; }
    python3 "$root/tools/bench_7zip.py" --label "$label-$i" "$log"
    logs+=("$log")
done
python3 "$root/tools/bench_7zip.py" --label "$label median" --median "${logs[@]}"
