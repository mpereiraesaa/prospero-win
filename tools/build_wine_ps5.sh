#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Cross-build Wine's Unix side (ntdll.so, win32u.so, wineserver) for the PS5.
#
# The pinned Wine revision is copied into a private work tree, the patch
# series in wine/patches is applied in numeric order, and the copy is
# configured for FreeBSD with the PS5 payload SDK: the PS5 compiler defines
# __FreeBSD__ and ships FreeBSD headers, so Wine takes its FreeBSD paths
# (kqueue, sysctl) and __PROSPERO__ selects the PS5 patches. PE modules come
# from the host build (tools/build_wine_runtime.sh), whose tools/ directory
# also supplies winebuild and widl, except the few a patch changes
# (PE_TARGETS), which are built here from the patched tree for both
# architectures into <work>/pe.
#
# Patch numbers are owned by range: 0100-0499 services, loader and
# presentation; 0500-0899 execution core (signals, TEB, virtual memory);
# 0900-0999 shared clock and input fast paths.
#
# Two links follow. The ELF link builds Wine's own targets against the
# payload SDK and reports, rather than fails on, unresolved symbols. The PRX
# link is what the console loads: ntdll, win32u and the in-process
# wineserver (patch 0110) linked against the
# title's stub libraries with the PS5 shims in wine/ps5 and a generated
# export descriptor, then converted and signed with ps5-native-tool into
# <work>/prx/sce_module. Both are measured in <work>/report.json. Module
# conversion needs a foundation with module export publishing and name-form
# export hashes (its branch exp/prx-module, MODULE_EXPORTS_COMMIT); the
# title's pinned foundation predates it, so --prx-foundation names that checkout, and without it the
# PRX link is skipped and says why.
#
# Vulkan (patches 0450-0460): win32u dlopens libvulkan.so, which pw_wine_dl
# turns into libvulkan.prx, the PS5 Vulkan driver (ps5vk) linked from its SDK
# (--ps5vk-sdk, a dist-sdk directory: lib/libps5vk.a, lib/libpsbc.a and the
# AGC import facades). Without it libvulkan.prx is skipped and Vulkan
# reports no driver; winevulkan.prx, Wine's Vulkan Unix side, is built
# either way. ps5vk is GPL-3.0-or-later: a title that ships libvulkan.prx
# ships a GPL work (see its SDK's LICENSE). --radv DIR links RADV (Mesa's
# AMD driver, MIT) instead: DIR is a PS5_Vulkan checkout whose
# tools/build-radv.sh release has built its pinned PS5_Mesa revision, linked
# by tools/link_radv_prx.sh. The two options are exclusive.
# OpenGL (patch 0660): --ps5-opengl-sdk names an installed
# installed ps5-opengl SDK prefix. Its GPL-3.0-or-later static archive is linked into
# win32u.prx; distributed builds must preserve the SDK's source and license
# obligations. SDK 0.6.0 provides a tested OpenGL compatibility profile for
# legacy WGL contexts, as well as the OpenGL 4.6 Core profile.
#
# Usage:
#   tools/build_wine_ps5.sh [--check-patches] [--patches DIR] [--work DIR]
#       [--source DIR] [--host-tools DIR] [--foundation DIR] [--sdk DIR]
#       [--prx-foundation DIR] [--ps5vk-sdk DIR | --radv DIR]
#       [--ps5-opengl-sdk DIR] [--jobs N]
set -eu

WINE_COMMIT=490f6d5dcbb2a5047345b8af88d114bbcaad69a8
MODULE_EXPORTS_COMMIT=30597512539e7edfde079cbcaf4a626bc0a948c5
# FreeType draws Wine's text. It is built from a pinned release with the
# payload SDK and linked into libfreetype.prx, which Wine's own
# dlopen("libfreetype.so") loads through pw_wine_dl, next to ntdll.prx.
FREETYPE_VERSION=2.13.3
FREETYPE_SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
FREETYPE_URL=https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VERSION.tar.xz
# Only what Wine's fonts need: TrueType/CFF/Type 1 outlines, Windows .fon
# bitmaps, hinting and the two rasterisers; gzip (FreeType's own zlib copy)
# for the compressed WOFF tables sfnt reads.
FREETYPE_UNITS="base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbbox base/ftbitmap
 base/ftglyph base/ftmm base/ftsynth base/fttype1 base/ftfstype base/ftgasp base/ftwinfnt
 autofit/autofit truetype/truetype type1/type1 cff/cff winfonts/winfnt psaux/psaux
 psnames/psnames pshinter/pshinter sfnt/sfnt smooth/smooth raster/raster gzip/ftgzip"
# The functions dlls/win32u/freetype.c and dlls/dwrite/freetype.c load with
# dlsym.
FREETYPE_EXPORTS="FT_Activate_Size FT_Done_Face FT_Done_FreeType FT_Done_Glyph FT_Done_Size
 FT_Get_Glyph FT_Get_Kerning FT_Glyph_Copy FT_Glyph_Get_CBox FT_Glyph_Transform FT_New_Size
 FT_Outline_Copy FT_Outline_Decompose FT_Outline_Done FT_Outline_EmboldenXY FT_Outline_New
 FT_Get_Char_Index FT_Get_First_Char FT_Get_Next_Char
 FT_Get_Sfnt_Name FT_Get_Sfnt_Name_Count FT_Get_Sfnt_Table FT_Get_TrueType_Engine_Type
 FT_Get_WinFNT_Header FT_Init_FreeType FT_Library_SetLcdFilter FT_Library_Version
 FT_Load_Glyph FT_Load_Sfnt_Table FT_Matrix_Multiply FT_MulDiv FT_MulFix FT_New_Face
 FT_New_Memory_Face FT_Outline_Embolden FT_Outline_Get_Bitmap FT_Outline_Get_CBox
 FT_Outline_Transform FT_Outline_Translate FT_Property_Set FT_Render_Glyph FT_Set_Charmap
 FT_Set_Pixel_Sizes FT_Vector_Length FT_Vector_Transform FT_Vector_Unit"
TARGETS="dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver dlls/winevulkan/winevulkan.so
 dlls/opengl32/opengl32.so dlls/ws2_32/ws2_32.so dlls/crypt32/crypt32.so dlls/dwrite/dwrite.so"
# The PE modules the patches change: every xinput built from xinput1_3's
# source reads the title's controller (patch 0470); xinput9_1_0 forwards to
# xinput1_4. quartz: its renderers wait for a state change without the filter
# lock (patch 0700). opengl32: it batches immediate-mode calls for its Unix
# side (patch 0720), so its PE and Unix halves must come from the same build.
# winevulkan: emit its PE thunks alongside the Unix side so command-stream
# hooks and dispatch table capability checks come from the same source.
PE_MODULES="ntdll win32u xinput1_1 xinput1_2 xinput1_3 xinput1_4 xinputuap quartz opengl32 winevulkan"
# Everything optional but FreeType (built below) is off: the console has none
# of these libraries, and a configure-time probe against the payload SDK must
# not pick up host headers.
CONFIGURE_ARGS="--host=x86_64-unknown-freebsd11 --build=x86_64-pc-linux-gnu
 --enable-archs=i386,x86_64 --disable-tests --without-x
 --without-fontconfig --without-gnutls --without-alsa --without-pulse
 --without-dbus --without-gstreamer --without-sdl --without-udev --without-usb
 --without-v4l2 --without-wayland --without-opengl --without-oss
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
ps5vk_sdk=${PS5VK_SDK:-}
ps5opengl_sdk=${PS5_OPENGL_SDK:-}
ps5opengl_lib=
radv=${PROSPERO_RADV:-}
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
    --ps5vk-sdk) ps5vk_sdk=$2; shift ;;
    --ps5-opengl-sdk) ps5opengl_sdk=$2; shift ;;
    --radv) radv=$2; shift ;;
    --jobs) jobs=$2; shift ;;
    *) fail "unknown argument $1" ;;
    esac
    shift
done

[ -z "$ps5vk_sdk" ] || [ -z "$radv" ] || fail "--ps5vk-sdk and --radv both name libvulkan.prx; give one"
[ -z "$ps5opengl_sdk" ] || [ -f "$ps5opengl_sdk/lib/libPS5OpenGL.a" ] ||
    [ -f "$ps5opengl_sdk/lib/libPS5OpenGLCore33.a" ] ||
    fail "no PS5 OpenGL SDK archive at $ps5opengl_sdk"
[ -z "$ps5opengl_sdk" ] || [ -f "$ps5opengl_sdk/manifest.sha256" ] ||
    fail "PS5 OpenGL SDK is missing manifest.sha256"
if [ -n "$ps5opengl_sdk" ]; then
    if [ -f "$ps5opengl_sdk/lib/libPS5OpenGL.a" ]; then
        ps5opengl_lib=PS5OpenGL
    else
        ps5opengl_lib=PS5OpenGLCore33
    fi
fi
[ -z "$ps5opengl_sdk" ] ||
    (cd "$ps5opengl_sdk" && sha256sum --status --check manifest.sha256) ||
    fail "PS5 OpenGL SDK does not match its SHA-256 manifest"

# The series: NNNN-lower-case-name.patch, unique numbers below 1000, each a
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
        [ "$number" -ge 100 ] && [ "$number" -le 999 ] ||
            fail "patch number outside 0100-0999: $patch"
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

# FreeType, once per pinned release: a static archive for configure and the
# objects the PRX stage links. PROSPERO_FREETYPE_TARBALL names a local copy.
ft=$work/freetype
ft_stamp=$(printf '%s\n' "$FREETYPE_SHA256" "$FREETYPE_UNITS" | sha256sum | cut -c1-64)
if [ "$(cat "$ft/.prospero-stamp" 2>/dev/null)" != "$ft_stamp" ]; then
    rm -rf "$ft"
    mkdir -p "$ft/src" "$ft/obj" "$ft/config"
    tarball=${PROSPERO_FREETYPE_TARBALL:-$ft/freetype-$FREETYPE_VERSION.tar.xz}
    [ -f "$tarball" ] || curl -sSfL -o "$tarball" "$FREETYPE_URL" ||
        fail "cannot download $FREETYPE_URL"
    [ "$(sha256sum "$tarball" | cut -c1-64)" = "$FREETYPE_SHA256" ] ||
        fail "$tarball does not match FreeType $FREETYPE_VERSION's pinned SHA-256"
    tar -xJf "$tarball" -C "$ft/src" --strip-components=1
    cat > "$ft/config/ftmodule_ps5.h" <<'EOF'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, winfnt_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
EOF
    for unit in $FREETYPE_UNITS; do
        "$sdk/bin/prospero-clang" -std=c99 -O2 -fPIC -DFT2_BUILD_LIBRARY \
            '-DFT_CONFIG_MODULES_H=<ftmodule_ps5.h>' -I"$ft/config" -I"$ft/src/include" \
            -c "$ft/src/src/$unit.c" -o "$ft/obj/$(basename "$unit").o" ||
            fail "cannot compile FreeType $unit.c"
    done
    "$sdk/bin/llvm-ar" rcs "$ft/libfreetype.a" "$ft"/obj/*.o
    echo "$ft_stamp" > "$ft/.prospero-stamp"
    echo "built FreeType $FREETYPE_VERSION"
fi
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

# Stage Vulkan batching after the complete patch series. Failure must stop the
# build before either half can be emitted with a different dispatch table.
python3 "$root/tools/stage_vk_batch.py" --source "$tree" --repo "$root" ||
    fail "cannot stage Vulkan command-stream runtime"

# Stage the pointer-free shared-clock ABI used by PE32 and the provider.
cp "$root/wine/ps5/time/pw_qpc_clock.h" "$tree/dlls/ntdll/pw_qpc_clock.h" ||
    fail "cannot stage shared-clock ABI"
cp "$root/wine/ps5/input/pw_key_shared.h" "$tree/dlls/win32u/pw_key_shared.h" ||
    fail "cannot stage shared-input ABI"

# Shared bridge registry and checked native driver boundary.
for unit in pw_d3d9_window.c pw_d3d9_window.h pw_d3d9_window_driver.c pw_d3d9_window_driver.h; do
    cp "$root/wine/ps5/$unit" "$tree/dlls/win32u/$unit" || fail "cannot stage bridge window adapter"
done

# The PS5 OpenGL SDK is optional. When supplied, Wine's generic EGL/WGL
# frontend binds directly to its static EGL symbols and the win32u PRX links
# the SDK into the runtime.
opengl_cflags=${CFLAGS:--g -O2}
if [ -n "$ps5opengl_sdk" ]; then opengl_cflags="$opengl_cflags -DWINE_PS5_OPENGL"; fi

# Reconfigure whenever the patches, staged Vulkan sources or arguments change.
stamp=$(
    { printf '%s\n' "$WINE_COMMIT" "$CONFIGURE_ARGS" "$sdk" "$FREETYPE_SHA256" \
        "$ps5opengl_sdk" "$opengl_cflags"
      for patch in $ordered; do cat "$patches/$patch"; done
      cat "$root/tools/stage_vk_batch.py" "$root/tools/generate_vk_codecs.py" "$root"/wine/ps5/pw_vk_*.[ch] \
          "$root"/wine/ps5/vulkan/*.[ch] "$root/wine/ps5/time/pw_qpc_clock.h" "$root/wine/ps5/input/pw_key_shared.h" "$root"/wine/ps5/pw_d3d9_window*.[ch]; } | sha256sum | cut -c1-64)
build=$work/build
if [ ! -f "$build/Makefile" ] || [ "$(cat "$build/.prospero-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$build"
    mkdir -p "$build"
    # shellcheck disable=SC2086
    # FreeType is found by its flags; its soname is the name Wine dlopens,
    # which pw_wine_dl turns into libfreetype.prx beside ntdll.prx.
    (cd "$build" && "$tree/configure" $CONFIGURE_ARGS CC="$sdk/bin/prospero-clang" \
        CFLAGS="$opengl_cflags" \
        FREETYPE_CFLAGS="-I$ft/src/include" FREETYPE_LIBS="$ft/libfreetype.a" \
        ac_cv_lib_soname_freetype=libfreetype.so ac_cv_lib_soname_vulkan=libvulkan.so \
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
# ntdll's anonymous memory comes from direct memory (patch 0600).
dmem=""
for unit in pw_wine_dmem pw_wine_dmem_ps5; do
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
        -c "$root/wine/ps5/$unit.c" -o "$work/heap/$unit.o" || fail "cannot compile $unit.c"
    dmem="$dmem $work/heap/$unit.o"
done
base="-L$work/ps5lib -lunwind -Wl,--warn-unresolved-symbols"
status=0
: > "$work/make.log"
# LDFLAGS is not a make dependency, so relink the three targets every run.
(cd "$build" && rm -f $TARGETS)
for step in "dlls/ntdll/ntdll.so|$heap $dmem" "dlls/win32u/win32u.so|" "server/wineserver|$heap" \
        "dlls/winevulkan/winevulkan.so|" "dlls/opengl32/opengl32.so|" "dlls/ws2_32/ws2_32.so|" \
        "dlls/crypt32/crypt32.so|" "dlls/dwrite/dwrite.so|"; do
    target=${step%%|*}; objects=${step#*|}
    make -C "$build" -k -j"$jobs" LDFLAGS="$objects $base" "$target" \
        >> "$work/make.log" 2>&1 || status=$?
done
# The patched PE modules, built by the PE cross compilers into <work>/pe.
rm -rf "$work/pe"
for arch in i386 x86_64; do
    mkdir -p "$work/pe/$arch-windows"
    modules="$PE_MODULES"
    [ "$arch" != x86_64 ] || modules="$modules wow64 wow64win"
    for module in $modules; do
        target=dlls/$module/$arch-windows/$module.dll
        make -C "$build" -k -j"$jobs" "$target" >> "$work/make.log" 2>&1 || status=$?
        [ ! -f "$build/$target" ] || cp "$build/$target" "$work/pe/$arch-windows/"
    done
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
# The revisions behind the PRXs, for the release's SOURCES.txt
# (tools/package_release.sh): report.json's "sources".
source_prx_foundation=$(git -C "$prx_foundation" rev-parse HEAD 2>/dev/null || true)
source_ps5_mesa= source_ps5_vulkan= source_radv_payload_sdk= source_ps5vk=
source_ps5_opengl_sdk= source_ps5_opengl=
if [ -n "$ps5opengl_sdk" ]; then
    source_ps5_opengl_sdk=$(sha256sum "$ps5opengl_sdk/manifest.sha256" | cut -c1-64)
    # An SDK installed inside its ps5-opengl checkout (make sdk's
    # build/sdk/ps5-opengl-gl46) names the commit it was built from.
    source_ps5_opengl=$(git -C "$ps5opengl_sdk" rev-parse HEAD 2>/dev/null || true)
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
# link_prx NAME TARGET "EXTRA OBJECTS" ["IMPORTED MODULES"] ["EXTRA STUB DIR"];
# TARGET "-" takes only the extra objects (a module that is not one of
# Wine's own targets).
link_prx() {
    name=$1; objects=; stubs=
    for module in ${4:-}; do stubs="$stubs --stub $module"; done
    [ "$2" = - ] || objects=$(link_objects "$2")
    log=$prx/$name.link.log
    [ -n "$objects$3" ] || { echo "no ELF link of $2 in make.log" > "$log"; prx_status=1; return; }
    if [ "$name" = win32u ] && [ -n "$ps5opengl_sdk" ]; then
        opengl_stubs=$prx/opengl-stubs
        mkdir -p "$opengl_stubs"
        cp "$sdk"/target/lib/*.so "$opengl_stubs/"
        # A title has libkernel and libSceLibcInternal, not the WebKit
        # process's libkernel_web and libScePosixForWebKit: symbols bound to
        # those resolve to NULL in the title (pthread_getspecific in
        # win32u's first call). Leave them out so everything binds to the
        # title's own libraries, and a symbol only they have stays unresolved.
        rm -f "$opengl_stubs/libkernel_web.so" "$opengl_stubs/libScePosixForWebKit.so"
        for import in "$ps5opengl_sdk"/lib/*.so; do
            [ -e "$import" ] || continue
            [ -e "$opengl_stubs/$(basename "$import")" ] || cp "$import" "$opengl_stubs/"
        done
        # The OpenGL SDK's consumer graph includes C++ objects. Use the SDK's
        # C++ runtime archives, but do not pull a second copy of libc into
        # win32u: its allocator and system imports belong to the existing Wine
        # PRX/module contracts, and the SDK payload libc adds raw syscalls.
        # shellcheck disable=SC2086
        # Match the upstream Core33 link options: force its weak AGC Gate2
        # entry point into the archive so the draw submit path is available.
        (cd "$build" && "$sdk/bin/prospero-clang++" -shared -nodefaultlibs \
            -Wl,-Bsymbolic -Wl,-T,"$pie" -Wl,-T,"$root/wine/ps5/prx_eh_frame.ld" \
            -Wl,--eh-frame-hdr -Wl,-soname,"$name.prx" -Wl,-z,defs \
            -Wl,--warn-unresolved-symbols -L"$work/ps5lib" -L"$ps5opengl_sdk/lib" \
            -Wl,-u,ps5_agc_gate2_run -o "$prx/$name.shared.elf" $objects $3 ${4:-} \
            -Wl,--start-group -l"$ps5opengl_lib" -Wl,--end-group \
            -Wl,--start-group "$sdk/target/lib/libunwind.a" \
            "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libc++.a" -Wl,--end-group \
            -lSceAgc -lSceAgcDriver -lSceVideoOut -lkernel -lSceSystemService \
            -Wl,--as-needed "$opengl_stubs"/*.so) > "$log" 2>&1 &&
        stubs_dir=$opengl_stubs
    else
        # shellcheck disable=SC2086
        (cd "$build" && "$sdk/bin/prospero-lld" --shared -Bsymbolic -T "$pie" \
            -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr -soname "$name.prx" -z defs \
            --warn-unresolved-symbols -L"$work/ps5lib" -o "$prx/$name.shared.elf" $objects $3 ${4:-} \
            "$sdk/target/lib/libunwind.a" --as-needed "${5:-$sdk/target/lib}"/*.so) > "$log" 2>&1
        stubs_dir=${5:-$sdk/target/lib}
    fi &&
    "$tool" link --module --in "$prx/$name.shared.elf" --out "$prx/$name.elf" \
        --stub-dir "$stubs_dir" $stubs --module-sdk 0x02000009 \
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
        pw_wine_set_pad pw_wine_rumble pw_wine_pad pw_wine_set_rumble \
        pw_wine_set_audio_sink pw_wine_audio_available pw_wine_audio_output \
        __wine_virtual_stats __wine_virtual_fault_top __wine_ps5_set_output_sink __wine_ps5_memory_stats pw_cwd_set \
        __wine_ps5_set_segv_hook __wine_ps5_set_segv_unresolved_hook pw_wine_set_display_release pw_wine_release_display \
        --optional-from "$build/dlls/ntdll/ntdll.so" --nm "$sdk/bin/prospero-nm" \
        --optional-export __wine_prospero_native_wow64_caps
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/win32u_desc.c" __wine_unix_lib_init
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wineserver_desc.c" \
        pw_wineserver_connect pw_wine_thread_register pw_wine_thread_unregister pw_wineserver_call_direct pw_wineserver_try_fast_mutex \
        pw_wineserver_mutex_backend \
        pw_wineserver_sync_backend \
        pw_cwd_share_changes
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wowprospero_desc.c" __wine_unix_call_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wineps5_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/xinput_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/winevulkan_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/opengl32_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/ws2_32_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/crypt32_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/dwrite_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/libvulkan_desc.c" \
        vkGetInstanceProcAddr vkGetDeviceProcAddr
    # shellcheck disable=SC2086
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/libfreetype_desc.c" $FREETYPE_EXPORTS
    for unit in ntdll_desc win32u_desc wineserver_desc wowprospero_desc wineps5_desc \
            libfreetype_desc xinput_desc winevulkan_desc opengl32_desc ws2_32_desc crypt32_desc dwrite_desc \
            libvulkan_desc; do
        "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$root/wine/ps5" \
            -c "$prx/obj/$unit.c" -o "$prx/obj/$unit.o" || fail "cannot compile $unit.c"
    done
    link_prx ntdll dlls/ntdll/ntdll.so "$heap $dmem $shims $prx/obj/ntdll_desc.o $cwd_wraps"
    if [ -n "$ps5opengl_sdk" ]; then
        # Mesa's embedded diagnostics name these libc APIs, but a title has no
        # process launcher or syslog daemon. Keep those paths inert and supply
        # only the SDK's TLS helper; do not pull its syscall-bearing libc.a.
        "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
            -c "$root/wine/ps5/pw_opengl_libc.c" -o "$prx/obj/pw_opengl_libc.o" ||
            fail "cannot compile OpenGL libc shims"
        (cd "$prx/obj" && "$sdk/bin/llvm-ar" x "$sdk/target/lib/libc.a" emutls.o) ||
            fail "no emutls.o in the payload SDK's libc.a"
        link_prx win32u dlls/win32u/win32u.so \
            "$prx/obj/win32u_desc.o $prx/obj/pw_opengl_libc.o $prx/obj/emutls.o" \
            "$prx/ntdll.shared.elf"
    else
        link_prx win32u dlls/win32u/win32u.so "$prx/obj/win32u_desc.o" "$prx/ntdll.shared.elf"
    fi
    # ntdll loads it with its own dlopen; it has its own heap, needs no
    # dlfcn of its own, and signals threads through the registry ntdll fills.
    link_prx wineserver server/wineserver "$heap $prx/obj/pw_wine_compat.o \
        $prx/obj/pw_wine_compat_libc.o $prx/obj/pw_wine_threads.o \
        $prx/obj/pw_wine_threads_libc.o $prx/obj/wineserver_desc.o \
        $prx/obj/pw_wine_cwd.o $prx/obj/pw_wine_cwd_libc.o $cwd_wraps"
    # The WoW64 CPU backend (wine/wowprospero): its Unix library runs i386
    # code through the IA-32 DBT and imports ntdll's functions like win32u.
    wow=""
    for unit in wine/wowprospero/unix.c src/pw_x86_engine.c src/pw_x86_block.c \
            src/pw_x86_cache.c src/pw_x86_hostexec.c src/pw_x86_reencode.c src/pw_x87.c \
            src/pw_guest_fp.c src/pw_vm.c src/pw_vm_posix.c; do
        object=$prx/obj/wow_$(basename "$unit" .c).o
        "$sdk/bin/prospero-clang" -std=gnu11 -O2 -fPIC -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT \
            -I"$root/wine/wowprospero" -I"$build/include" -I"$tree/include" \
            -I"$root/src" -I"$root/include" -c "$root/$unit" -o "$object" || fail "cannot compile $unit"
        wow="$wow $object"
    done
    # unix.c keeps one __thread pointer, which the PS5 compiler emulates;
    # the payload SDK's own emutls object provides __emutls_get_address.
    (cd "$prx/obj" && "$sdk/bin/llvm-ar" x "$sdk/target/lib/libc.a" emutls.o) ||
        fail "no emutls.o in the payload SDK's libc.a"
    link_prx wowprospero - "$wow $prx/obj/emutls.o $prx/obj/wowprospero_desc.o" "$prx/ntdll.shared.elf"
    link_prx libfreetype - "$ft/obj/*.o $prx/obj/libfreetype_desc.o"
    # The audio driver (wine/wineps5): mmdevapi loads it by name, and it
    # mixes into the title's audio sink, which ntdll exports.
    # mmdevapi's interfaces are IDL; the ELF link built only what ntdll,
    # win32u and the server include.
    make -C "$build" include/audioclient.h include/mmdeviceapi.h include/devicetopology.h \
        include/propsys.h include/propidl.h include/objidl.h include/objidlbase.h \
        include/unknwn.h include/wtypes.h include/oaidl.h >> "$work/make.log" 2>&1 ||
        fail "cannot generate mmdevapi's headers; see $work/make.log"
    audio=""
    for unit in wine/wineps5/unix.c src/pw_audio_mix.c; do
        object=$prx/obj/wineps5_$(basename "$unit" .c).o
        "$sdk/bin/prospero-clang" -std=gnu11 -O2 -fPIC -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT \
            -I"$build/include" -I"$tree/include" -I"$tree/dlls/mmdevapi" -I"$root/wine/ps5" \
            -I"$root/src" -I"$root/include" -c "$root/$unit" -o "$object" || fail "cannot compile $unit"
        audio="$audio $object"
    done
    link_prx wineps5 - "$audio $prx/obj/wineps5_desc.o" "$prx/ntdll.shared.elf"
    # xinput's Unix library (patch 0470), which every xinput loads by name:
    # it reads the title's controller through ntdll's dlsym.
    "$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Werror -fPIC -D__WINESRC__ -DWINE_UNIX_LIB \
        -D_REENTRANT -I"$tree/dlls/xinput1_3" -I"$build/include" -I"$tree/include" \
        -c "$tree/dlls/xinput1_3/unixlib.c" -o "$prx/obj/xinput_unixlib.o" ||
        fail "cannot compile xinput's unixlib.c"
    link_prx xinput1_3 - "$prx/obj/xinput_unixlib.o $prx/obj/xinput_desc.o" "$prx/ntdll.shared.elf"
    # Wine's Vulkan Unix side: it calls into ntdll and into win32u's Vulkan
    # driver (__wine_get_vulkan_driver).
    link_prx winevulkan dlls/winevulkan/winevulkan.so "$prx/obj/winevulkan_desc.o" \
        "$prx/ntdll.shared.elf $prx/win32u.shared.elf"
    # OpenGL's Unix side, without OpenGL (the console has none): wined3d, which
    # Wine's d3d10.dll imports even over DXVK's d3d10core, needs opengl32 to
    # initialise, and it does so with no driver.
    link_prx opengl32 dlls/opengl32/opengl32.so "$prx/obj/opengl32_desc.o" \
        "$prx/ntdll.shared.elf $prx/win32u.shared.elf"
    # Winsock's Unix side: games import ws2_32 even when they never go online
    # (Warcraft III's does), and it does not initialise without it.
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
        -c "$root/wine/ps5/pw_ws2_32_libc.c" -o "$prx/obj/pw_ws2_32_libc.o" ||
        fail "cannot compile pw_ws2_32_libc.c"
    link_prx ws2_32 dlls/ws2_32/ws2_32.so \
        "$prx/obj/ws2_32_desc.o $prx/obj/pw_ws2_32_libc.o $prx/obj/emutls.o" "$prx/ntdll.shared.elf"
    # CryptoAPI's Unix side: crypt32 does not initialise without it, and
    # FFmpeg's avformat imports crypt32 (LAV Filters, the DirectShow filters
    # Warcraft III's cinematics play through). Without GnuTLS it keeps what a
    # game needs: stores, certificates, hashes; PFX import reports failure.
    link_prx crypt32 dlls/crypt32/crypt32.so "$prx/obj/crypt32_desc.o" "$prx/ntdll.shared.elf"
    # DirectWrite's Unix side, FreeType through libfreetype.prx: without it
    # every glyph measurement a DirectWrite client makes calls through a
    # NULL table, and Chromium (Battle.net's login page) aborts laying out
    # text.
    link_prx dwrite dlls/dwrite/dwrite.so "$prx/obj/dwrite_desc.o" "$prx/ntdll.shared.elf"
    # The Vulkan driver itself, from ps5vk's SDK: only what its two entry
    # points reach, since the archive repeats a member. Its import facades
    # join the SDK's stubs.
    if [ -n "$ps5vk_sdk" ] && [ -f "$ps5vk_sdk/lib/libps5vk.a" ]; then
        mkdir -p "$prx/vkstubs"
        cp "$sdk"/target/lib/*.so "$ps5vk_sdk"/lib/libSceAgc*.so "$prx/vkstubs/"
        "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC \
            -c "$root/wine/ps5/pw_vulkan_libc.c" -o "$prx/obj/pw_vulkan_libc.o" ||
            fail "cannot compile pw_vulkan_libc.c"
        link_prx libvulkan - "-S -u vkGetInstanceProcAddr -u vkGetDeviceProcAddr \
            $prx/obj/libvulkan_desc.o $prx/obj/pw_vulkan_libc.o $prx/obj/emutls.o \
            $ps5vk_sdk/lib/libps5vk.a $ps5vk_sdk/lib/libpsbc.a $sdk/target/lib/libc++.a \
            $sdk/target/lib/libc++abi.a" "" "$prx/vkstubs"
        vulkan_status="libvulkan.prx from $ps5vk_sdk"
        source_ps5vk=$(sha256sum "$ps5vk_sdk/lib/libps5vk.a" | cut -c1-64)
    elif [ -n "$radv" ]; then
        # RADV, from a PS5_Vulkan checkout's release archive and its own link
        # recipe (tools/link_radv_prx.sh).
        if revision=$(bash "$root/tools/link_radv_prx.sh" "$radv" "$prx" "$tool" "$pie"); then
            vulkan_status="libvulkan.prx from RADV, PS5_Mesa $revision"
            source_ps5_mesa=$revision
            source_ps5_vulkan=$(git -C "$radv" rev-parse HEAD 2>/dev/null || true)
            source_radv_payload_sdk=$(cat "$radv/.deps/native/ps5-payload-sdk/.ps5-sdk-revision" \
                2>/dev/null || true)
        else
            prx_status=1
            vulkan_status="RADV link failed (see $prx/libvulkan.link.log)"
        fi
    else
        vulkan_status="skipped: no --ps5vk-sdk with lib/libps5vk.a, and no --radv"
    fi
    echo "vulkan: $vulkan_status"
    # Wine's own fonts, staged under share/wine/fonts beside the runtime.
    mkdir -p "$prx/fonts"
    cp "$tree"/fonts/*.ttf "$prx/fonts/"
fi

if PW_SOURCE_PRX_FOUNDATION=${source_prx_foundation:-} PW_SOURCE_PS5_MESA=${source_ps5_mesa:-} \
    PW_SOURCE_PS5_VULKAN=${source_ps5_vulkan:-} PW_SOURCE_RADV_PAYLOAD_SDK=${source_radv_payload_sdk:-} \
    PW_SOURCE_PS5VK=${source_ps5vk:-} PW_SOURCE_PS5_OPENGL_SDK=${source_ps5_opengl_sdk:-} \
    PW_SOURCE_PS5_OPENGL=${source_ps5_opengl:-} \
    python3 - "$build" "$work/make.log" "$work/report.json" "$sdk" "$WINE_COMMIT" "$prx" "$prx_status" \
    $ordered <<'PY'
import hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path
build, log, report, sdk, commit, prx, prx_status = sys.argv[1:8]
patches = sys.argv[8:]
text = Path(log).read_text(errors="replace")
owners = {"dlls/ntdll/": "dlls/ntdll/ntdll.so", "dlls/win32u/": "dlls/win32u/win32u.so",
          "server/": "server/wineserver", "dlls/winevulkan/": "dlls/winevulkan/winevulkan.so",
          "dlls/opengl32/": "dlls/opengl32/opengl32.so", "dlls/ws2_32/": "dlls/ws2_32/ws2_32.so",
          "dlls/crypt32/": "dlls/crypt32/crypt32.so", "dlls/dwrite/": "dlls/dwrite/dwrite.so"}
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
webkit_exports = exports("libkernel_web.so") | exports("libScePosixForWebKit.so")
objdump = shutil.which("llvm-objdump-18") or shutil.which("llvm-objdump") or f"{sdk}/bin/llvm-objdump"
SYSCALL_ALLOWED = {"__wine_syscall_dispatcher", "__wine_unix_call_dispatcher"}
ntdll_exports = exports("ntdll.shared.elf", prx) if (Path(prx) / "ntdll.shared.elf").is_file() else set()
for name in ("ntdll", "win32u", "wineserver", "wowprospero", "wineps5", "libfreetype", "xinput1_3",
             "winevulkan", "opengl32", "ws2_32", "crypt32", "dwrite", "libvulkan") if not prx_status.startswith("skipped") else ():
    module = Path(prx) / "sce_module" / f"{name}.prx"
    link_log = Path(prx) / f"{name}.link.log"
    if name == "libvulkan" and not link_log.is_file():
        continue    # neither a ps5vk SDK nor RADV given
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
        provided = title_exports | (ntdll_exports if name in ("win32u", "wowprospero", "wineps5",
                                                              "xinput1_3", "winevulkan", "opengl32", "ws2_32",
                                                              "crypt32", "dwrite") else set())
        entry["title_unbound"] = sorted((imports & exports("libkernel_sys.so")) - provided)
        # Nor does it get the WebKit process's libraries: ws2_32's getaddrinfo,
        # bound to libScePosixForWebKit, jumped to 0 in GTA IV (measured).
        entry["webkit_unbound"] = sorted((imports & webkit_exports) - provided)
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
# The patched PE modules, beside the PRXs.
result["pe"] = {str(path.relative_to(Path(prx).parent / "pe")): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted((Path(prx).parent / "pe").glob("*-windows/*.dll"))}
# Commits of the PRX foundation, PS5_Mesa, PS5_Vulkan, the payload SDK RADV
# was linked with and ps5-opengl; SHA-256 of the ps5vk archive and of the
# OpenGL SDK's manifest. Null when not linked or not recorded.
result["sources"] = {key: os.environ.get(f"PW_SOURCE_{key.upper()}") or None
                     for key in ("prx_foundation", "ps5_mesa", "ps5_vulkan", "radv_payload_sdk", "ps5vk",
                                 "ps5_opengl_sdk", "ps5_opengl")}
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
    if entry.get("webkit_unbound"):
        print(f"  WARNING: unbound in a title (WebKit libraries only): {','.join(entry['webkit_unbound'])}")
    if entry.get("raw_syscalls"):
        print(f"  raw syscall instructions (fatal in a title): {','.join(entry['raw_syscalls'])}")
print(f"pe: {', '.join(result['pe']) or 'none'}")
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
