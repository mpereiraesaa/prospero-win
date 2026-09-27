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
# presentation; 0500-0899 execution core (signals, TEB, virtual memory).
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
# Usage:
#   tools/build_wine_ps5.sh [--check-patches] [--patches DIR] [--work DIR]
#       [--source DIR] [--host-tools DIR] [--foundation DIR] [--sdk DIR]
#       [--prx-foundation DIR] [--jobs N]
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
# The functions dlls/win32u/freetype.c loads with dlsym.
FREETYPE_EXPORTS="FT_Done_Face FT_Get_Char_Index FT_Get_First_Char FT_Get_Next_Char
 FT_Get_Sfnt_Name FT_Get_Sfnt_Name_Count FT_Get_Sfnt_Table FT_Get_TrueType_Engine_Type
 FT_Get_WinFNT_Header FT_Init_FreeType FT_Library_SetLcdFilter FT_Library_Version
 FT_Load_Glyph FT_Load_Sfnt_Table FT_Matrix_Multiply FT_MulDiv FT_MulFix FT_New_Face
 FT_New_Memory_Face FT_Outline_Embolden FT_Outline_Get_Bitmap FT_Outline_Get_CBox
 FT_Outline_Transform FT_Outline_Translate FT_Property_Set FT_Render_Glyph FT_Set_Charmap
 FT_Set_Pixel_Sizes FT_Vector_Length FT_Vector_Transform FT_Vector_Unit"
TARGETS="dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver"
# The PE modules the patches change: every xinput built from xinput1_3's
# source reads the title's controller (patch 0470); xinput9_1_0 forwards to
# xinput1_4.
PE_MODULES="xinput1_1 xinput1_2 xinput1_3 xinput1_4 xinputuap"
# Everything optional but FreeType (built below) is off: the console has none
# of these libraries, and a configure-time probe against the payload SDK must
# not pick up host headers.
CONFIGURE_ARGS="--host=x86_64-unknown-freebsd11 --build=x86_64-pc-linux-gnu
 --enable-archs=i386,x86_64 --disable-tests --without-x
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

# Reconfigure whenever the patches or the arguments change.
stamp=$(
    { printf '%s\n' "$WINE_COMMIT" "$CONFIGURE_ARGS" "$sdk" "$FREETYPE_SHA256"
      for patch in $ordered; do cat "$patches/$patch"; done; } | sha256sum | cut -c1-64)
build=$work/build
if [ ! -f "$build/Makefile" ] || [ "$(cat "$build/.prospero-stamp" 2>/dev/null)" != "$stamp" ]; then
    rm -rf "$build"
    mkdir -p "$build"
    # shellcheck disable=SC2086
    # FreeType is found by its flags; its soname is the name Wine dlopens,
    # which pw_wine_dl turns into libfreetype.prx beside ntdll.prx.
    (cd "$build" && "$tree/configure" $CONFIGURE_ARGS CC="$sdk/bin/prospero-clang" \
        FREETYPE_CFLAGS="-I$ft/src/include" FREETYPE_LIBS="$ft/libfreetype.a" \
        ac_cv_lib_soname_freetype=libfreetype.so \
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
for step in "dlls/ntdll/ntdll.so|$heap $dmem" "dlls/win32u/win32u.so|" "server/wineserver|$heap"; do
    target=${step%%|*}; objects=${step#*|}
    make -C "$build" -k -j"$jobs" LDFLAGS="$objects $base" "$target" \
        >> "$work/make.log" 2>&1 || status=$?
done
# The patched PE modules, built by the PE cross compilers into <work>/pe.
rm -rf "$work/pe"
for arch in i386 x86_64; do
    mkdir -p "$work/pe/$arch-windows"
    for module in $PE_MODULES; do
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
# link_prx NAME TARGET "EXTRA OBJECTS" [IMPORTED MODULE]; TARGET "-" takes
# only the extra objects (a module that is not one of Wine's own targets).
link_prx() {
    name=$1; objects=
    [ "$2" = - ] || objects=$(link_objects "$2")
    log=$prx/$name.link.log
    [ -n "$objects$3" ] || { echo "no ELF link of $2 in make.log" > "$log"; prx_status=1; return; }
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
        pw_wine_set_pad pw_wine_rumble pw_wine_pad pw_wine_set_rumble \
        pw_wine_set_audio_sink pw_wine_audio_available pw_wine_audio_output \
        __wine_virtual_stats __wine_ps5_set_output_sink __wine_ps5_memory_stats
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/win32u_desc.c" __wine_unix_lib_init
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wineserver_desc.c" \
        pw_wineserver_connect pw_wine_thread_register
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wowprospero_desc.c" __wine_unix_call_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/wineps5_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/xinput_desc.c" \
        __wine_unix_call_funcs __wine_unix_call_wow64_funcs
    # shellcheck disable=SC2086
    python3 "$root/tools/gen_prx_descriptor.py" "$prx/obj/libfreetype_desc.c" $FREETYPE_EXPORTS
    for unit in ntdll_desc win32u_desc wineserver_desc wowprospero_desc wineps5_desc \
            libfreetype_desc xinput_desc; do
        "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$root/wine/ps5" \
            -c "$prx/obj/$unit.c" -o "$prx/obj/$unit.o" || fail "cannot compile $unit.c"
    done
    link_prx ntdll dlls/ntdll/ntdll.so "$heap $dmem $shims $prx/obj/ntdll_desc.o $cwd_wraps"
    link_prx win32u dlls/win32u/win32u.so "$prx/obj/win32u_desc.o" "$prx/ntdll.shared.elf"
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
    # Wine's own fonts, staged under share/wine/fonts beside the runtime.
    mkdir -p "$prx/fonts"
    cp "$tree"/fonts/*.ttf "$prx/fonts/"
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
for name in ("ntdll", "win32u", "wineserver", "wowprospero", "wineps5", "libfreetype", "xinput1_3") if not prx_status.startswith("skipped") else ():
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
        provided = title_exports | (ntdll_exports if name in ("win32u", "wowprospero", "wineps5",
                                                              "xinput1_3") else set())
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
# The patched PE modules, beside the PRXs.
result["pe"] = {str(path.relative_to(Path(prx).parent / "pe")): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted((Path(prx).parent / "pe").glob("*-windows/*.dll"))}
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
