#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Offline PE32 and PE64 WGL/Zink build. Supply the pinned source and compiler.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MESA_COMMIT=e4607ef697fe3df8d1ebc329bf1917d041843306
LLVM_MINGW_VERSION=20260922-ucrt
LLVM_MINGW_SHA256=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
work= mesa= archive= jobs=1
fail() { echo "build_mesa_zink: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
 case $1 in
 --work) work=$2; shift 2;;
 --mesa) mesa=$2; shift 2;;
 --llvm-mingw) archive=$2; shift 2;;
 --jobs) jobs=$2; shift 2;;
 *) fail "unknown argument $1";;
 esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || fail '--jobs must be positive'
[ -n "$work" ] && [ -n "$mesa" ] && [ -f "$archive" ] || fail 'requires --work DIR --mesa GIT_DIR --llvm-mingw ARCHIVE'
mesa=$(realpath "$mesa"); archive=$(realpath "$archive")
printf '%s  %s\n' "$LLVM_MINGW_SHA256" "$archive" | sha256sum --check --status || fail 'compiler archive hash mismatch'
git -C "$mesa" cat-file -e "$MESA_COMMIT^{commit}" || fail 'source lacks pinned Mesa commit'
mkdir -p "$work"; work=$(realpath "$work")
# Export immutable source, never build or change the supplied checkout.
if [ ! -f "$work/source/.revision" ]; then
 staging=$(mktemp -d "$work/source.XXXXXX")
 git -C "$mesa" archive "$MESA_COMMIT" | tar -x -C "$staging"
 printf '%s\n' "$MESA_COMMIT" > "$staging/.revision"
 mv "$staging" "$work/source"
fi
[ "$(cat "$work/source/.revision")" = "$MESA_COMMIT" ] || fail 'work source pin differs; use a new work directory'
python3 "$root/tools/mesa_zink_manifest.py" --verify-source "$work/source" "$mesa" "$MESA_COMMIT"
[ "$(cat "$work/source/VERSION")" = 26.2.0 ] || fail 'unexpected Mesa version'
if [ ! -f "$work/toolchain/.archive-sha256" ]; then
 staging=$(mktemp -d "$work/toolchain.XXXXXX")
 tar -xJf "$archive" -C "$staging" --strip-components=1
 printf '%s\n' "$LLVM_MINGW_SHA256" > "$staging/.archive-sha256"
 mv "$staging" "$work/toolchain"
fi
[ "$(cat "$work/toolchain/.archive-sha256")" = "$LLVM_MINGW_SHA256" ] || fail 'work compiler pin differs'
export PATH="$work/toolchain/bin:$PATH"
meson=${MESON:-meson}
for arch in i686 x86_64; do
 family=x86; destination=i386-windows
 if [ "$arch" = x86_64 ]; then family=x86_64; destination=x86_64-windows; fi
 cross=$work/cross-$arch.ini
 cat > "$cross" <<CROSS
[binaries]
c = '$work/toolchain/bin/$arch-w64-mingw32-clang'
cpp = '$work/toolchain/bin/$arch-w64-mingw32-clang++'
ar = '$work/toolchain/bin/llvm-ar'
strip = '$work/toolchain/bin/llvm-strip'
windres = '$work/toolchain/bin/llvm-windres'
pkg-config = 'false'
[host_machine]
system = 'windows'
cpu_family = '$family'
cpu = '$arch'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
 build=$work/build-$arch
 reconfigure=(); [ ! -f "$build/build.ninja" ] || reconfigure=(--reconfigure)
 "$meson" setup "${reconfigure[@]}" "$build" "$work/source" --cross-file "$cross" \
  --wrap-mode=nofallback --buildtype=release --default-library=static \
  -Dplatforms=windows -Dgallium-drivers=zink -Dvulkan-drivers= -Dopengl=true \
  -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dllvm=disabled -Dzlib=disabled -Dzstd=disabled -Dshader-cache=disabled \
  -Dxmlconfig=disabled -Dexpat=disabled -Dlibunwind=disabled -Dvalgrind=disabled \
  -Dvideo-codecs= -Dbuild-tests=false -Dtools= -Dgallium-va=disabled -Dgallium-rusticl=false
 ninja -C "$build" -j "$jobs" src/gallium/targets/libgl-gdi/opengl32.dll src/gallium/targets/wgl/libgallium_wgl.dll
 mkdir -p "$work/artifacts/$destination"
 for file in "$build/src/gallium/targets/libgl-gdi/opengl32.dll" "$build/src/gallium/targets/wgl/libgallium_wgl.dll"; do
  # Strip a copy: preserve the original DLL for symbols and source association.
  "$work/toolchain/bin/llvm-strip" --strip-debug -o "$work/artifacts/$destination/${file##*/}" "$file"
 done
 # Copy only compiler runtimes actually imported, including their dependencies.
 # Wine provides the Win32/UCRT APIs; retain that existing platform ABI.
 python3 "$root/tools/mesa_zink_manifest.py" --copy-runtime \
  "$work/artifacts/$destination" "$work/toolchain/$arch-w64-mingw32/bin" \
  "$work/toolchain/bin/llvm-readobj"
done
mkdir -p "$work/artifacts/LICENSES/mesa" "$work/artifacts/LICENSES/llvm-mingw"
cp "$work/source/docs/license.rst" "$work/artifacts/LICENSES/mesa/"
cp -R "$work/source/licenses" "$work/artifacts/LICENSES/mesa/"
python3 "$root/tools/mesa_zink_manifest.py" --notices "$work/source" "$work/artifacts/LICENSES/mesa/source-notices.txt"
cp "$work/toolchain/LICENSE.TXT" "$work/artifacts/LICENSES/llvm-mingw/"
cp -R "$work/toolchain/i686-w64-mingw32/share/mingw32" "$work/artifacts/LICENSES/llvm-mingw/"
python3 "$root/tools/mesa_zink_manifest.py" "$work" "$MESA_COMMIT" "$LLVM_MINGW_VERSION" "$LLVM_MINGW_SHA256" "$0"
