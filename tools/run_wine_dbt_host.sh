#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Run a profile-described PE32 application under a pinned host Wine WoW64
# build whose i386 CPU is the prospero-win DBT (tools/build_wowprospero.sh).
#
# The profile's `executable` and `working_directory` decide where the staged
# application tree is copied inside the prefix's drive_c; nothing else is
# searched. The prefix is created on first use. `--cpu native` selects Wine's
# own wow64cpu for a control run with identical inputs.
#
# The run passes when, within the time limit, a top-level window owned by the
# executable appears (X11 host) and the backend reported no untranslatable
# instruction, host fault or DBT error. The log is kept for inspection.
#
# Usage:
#   tools/run_wine_dbt_host.sh --profile FILE --stage DIR [--prefix DIR]
#       [--build DIR] [--seconds N] [--cpu dbt|native] [--screenshot PNG]
#       [--log FILE]
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${PROSPERO_WINE_BUILD:-$root/.deps/wine/build}
prefix=$root/.deps/wine-dbt-prefix
seconds=45
cpu=dbt
profile=
stage=
screenshot=
log=

fail() { echo "run_wine_dbt_host: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case $1 in
    --profile) profile=$2; shift 2 ;;
    --stage) stage=$2; shift 2 ;;
    --prefix) prefix=$2; shift 2 ;;
    --build) build_dir=$2; shift 2 ;;
    --seconds) seconds=$2; shift 2 ;;
    --cpu) cpu=$2; shift 2 ;;
    --screenshot) screenshot=$2; shift 2 ;;
    --log) log=$2; shift 2 ;;
    *) fail "unknown argument $1" ;;
    esac
done
[ -f "$profile" ] || fail "--profile FILE is required"
[ -d "$stage" ] || fail "--stage DIR (the application tree) is required"
[ "$cpu" = dbt ] || [ "$cpu" = native ] || fail "--cpu must be dbt or native"
wine=$build_dir/wine
[ -x "$wine" ] || fail "no host Wine at $build_dir (run tools/build_wowprospero.sh)"
[ "$cpu" = native ] || [ -f "$build_dir/dlls/wowprospero/wowprospero.so" ] ||
    fail "wowprospero is not built into $build_dir"
[ -n "$log" ] || log=$prefix.log

# Exact executable and working directory from the profile, as drive_c paths.
eval "$(python3 - "$profile" <<'EOF'
import configparser, sys
config = configparser.ConfigParser(inline_comment_prefixes=None, comment_prefixes=(";", "#"))
config.read(sys.argv[1])
app = config["application"]
def unix(path):
    if not path[:3].upper() == "C:\\":
        raise SystemExit("profile paths must be absolute C:\\ paths")
    return path[3:].replace("\\", "/")
exe, cwd = unix(app["executable"]), unix(app["working_directory"])
if app.get("architecture") != "pe32":
    raise SystemExit("only pe32 profiles run through the i386 DBT backend")
print("exe_rel='%s'; cwd_rel='%s'; exe_name='%s'" % (exe, cwd, exe.rsplit("/", 1)[-1]))
EOF
)"

export WINEPREFIX=$prefix WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=${WINEDEBUG:-err+all}
if [ ! -f "$prefix/system.reg" ]; then
    "$wine" wineboot -i > "$prefix.boot.log" 2>&1 || fail "wineboot failed; see $prefix.boot.log"
fi
key='HKLM\Software\Microsoft\Wow64\x86'
if [ "$cpu" = dbt ]; then
    "$wine" reg add "$key" /ve /d wowprospero.dll /f > /dev/null 2>&1
else
    "$wine" reg delete "$key" /f > /dev/null 2>&1 || true
fi
"$build_dir/server/wineserver" -w

mkdir -p "$prefix/drive_c/$cwd_rel"
cp -R "$stage"/. "$prefix/drive_c/$cwd_rel/"
# Windows paths are case-insensitive; resolve the profile's executable name
# against the staged tree the same way.
exe_dir=$(dirname "$prefix/drive_c/$exe_rel")
exe_found=$(find "$exe_dir" -maxdepth 1 -type f -iname "$exe_name" | head -1)
[ -n "$exe_found" ] || fail "staged tree has no $exe_rel"
exe_name=$(basename "$exe_found")

cd "$prefix/drive_c/$cwd_rel"
timeout "$seconds" "$wine" "$exe_name" > "$log" 2>&1 &
pid=$!
window=
waited=0
while [ $waited -lt "$seconds" ] && kill -0 $pid 2>/dev/null; do
    sleep 1; waited=$((waited + 1))
    if [ -z "$window" ] && command -v xwininfo > /dev/null; then
        window=$(xwininfo -root -tree 2>/dev/null |
                 grep -i "(\"$exe_name\" " | grep -v "has no name" | awk '{print $1}' | head -1)
        if [ -n "$window" ] && [ -n "$screenshot" ]; then
            sleep 5; import -window "$window" "$screenshot" 2>/dev/null || true
        fi
    fi
done
"$build_dir/server/wineserver" -k 2>/dev/null || true
wait $pid 2>/dev/null || true

problems=$(grep -c "wow:BTCpuSimulate\|DBT error\|Unhandled exception" "$log" || true)
echo "run_wine_dbt_host: cpu=$cpu window=${window:-none} backend_errors=$problems log=$log"
[ -n "$window" ] && [ "$problems" -eq 0 ]
