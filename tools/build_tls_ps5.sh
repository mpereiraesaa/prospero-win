#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build the TLS library behind Wine's schannel for the console: nettle (with
# its own mini-gmp, so no GMP) and GnuTLS (with its included libtasn1 and
# libunistring; no p11-kit, IDN, TPM, zlib, brotli or zstd), as static
# archives the PS5 build links into libgnutls.prx (tools/build_wine_ps5.sh).
# The SDK has no libm, so an empty one stands in for the -lm the build asks.
#
# With them, the root certificates crypt32 trusts on the console (patch
# 0878): Mozilla's store as curl publishes it, a dated file pinned by
# SHA-256 like the tarballs, so a release carries a known bundle whoever
# builds it; and the licence texts of what libgnutls.prx links, which
# tools/package_release.sh ships (THIRD_PARTY.md says which covers what).
#
# Usage: tools/build_tls_ps5.sh [--work DIR] [--sdk DIR] [--jobs N]
#   work   .deps/wine-ps5/tls by default: tarballs, source trees and root/
#   sdk    the payload SDK (PS5_PAYLOAD_SDK, or the pinned foundation's)
# root/ holds lib/ and include/ for the Wine build, ca-certificates.crt and
# licenses/{gnutls,nettle}.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${PROSPERO_WINE_PS5_WORK:-$root/.deps/wine-ps5}/tls
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk}
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
NETTLE_VERSION=3.10.2
NETTLE_SHA256=fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5
NETTLE_URL=https://ftp.gnu.org/gnu/nettle/nettle-$NETTLE_VERSION.tar.gz
GNUTLS_VERSION=3.8.13
GNUTLS_SHA256=ffed8ec1bf09c2426d4f14aae377de4753b53e537d685e604e99a8b16ca9c97e
GNUTLS_URL=https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-$GNUTLS_VERSION.tar.xz
# Mozilla's root store, extracted by curl's mk-ca-bundle (MPL-2.0); the
# dated files at https://curl.se/docs/caextract.html do not change.
CA_BUNDLE_DATE=2026-09-25
CA_BUNDLE_SHA256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505
CA_BUNDLE_URL=https://curl.se/ca/cacert-$CA_BUNDLE_DATE.pem
fail() { echo "build_tls_ps5: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case $1 in
    --work) work=$2; shift ;;
    --sdk) sdk=$2; shift ;;
    --jobs) jobs=$2; shift ;;
    *) fail "unknown argument $1" ;;
    esac
    shift
done
[ -x "$sdk/bin/prospero-clang" ] || fail "no PS5 payload SDK at $sdk"
out=$work/root
mkdir -p "$out/lib" "$work"
fetch() {
    [ -f "$work/$2" ] || curl -sSfL -o "$work/$2" "$1" || fail "cannot download $1"
    [ "$(sha256sum "$work/$2" | cut -c1-64)" = "$3" ] || fail "$2 does not match its pinned SHA-256"
}
fetch "$NETTLE_URL" "nettle-$NETTLE_VERSION.tar.gz" "$NETTLE_SHA256"
fetch "$GNUTLS_URL" "gnutls-$GNUTLS_VERSION.tar.xz" "$GNUTLS_SHA256"
fetch "$CA_BUNDLE_URL" "cacert-$CA_BUNDLE_DATE.pem" "$CA_BUNDLE_SHA256"
[ -f "$out/lib/libm.a" ] || "$sdk/bin/llvm-ar" rcs "$out/lib/libm.a"
export CC="$sdk/bin/prospero-clang" CXX="$sdk/bin/prospero-clang++" AR="$sdk/bin/llvm-ar" \
       RANLIB="$sdk/bin/llvm-ranlib" NM="$sdk/bin/llvm-nm" STRIP=true \
       CFLAGS="-O2 -fPIC" LDFLAGS="-L$out/lib" \
       PKG_CONFIG_LIBDIR="$out/lib/pkgconfig" PKG_CONFIG_PATH="$out/lib/pkgconfig"
host=x86_64-unknown-freebsd11
if [ ! -f "$out/lib/libhogweed.a" ]; then
    rm -rf "$work/nettle-$NETTLE_VERSION"
    tar -xzf "$work/nettle-$NETTLE_VERSION.tar.gz" -C "$work"
    (cd "$work/nettle-$NETTLE_VERSION" &&
        ./configure --host=$host --prefix="$out" --enable-mini-gmp --disable-shared --enable-static \
            --disable-documentation --disable-assembler --disable-openssl > configure.log 2>&1 &&
        make -j"$jobs" > make.log 2>&1 && make install > install.log 2>&1) ||
        fail "nettle did not build; see $work/nettle-$NETTLE_VERSION/*.log"
    echo "built nettle $NETTLE_VERSION"
fi
if [ ! -f "$out/lib/libgnutls.a" ]; then
    rm -rf "$work/gnutls-$GNUTLS_VERSION"
    tar -xJf "$work/gnutls-$GNUTLS_VERSION.tar.xz" -C "$work"
    (cd "$work/gnutls-$GNUTLS_VERSION" &&
        ./configure --host=$host --prefix="$out" --disable-shared --enable-static \
            --with-included-libtasn1 --with-included-unistring --without-p11-kit --without-idn \
            --without-tpm --without-tpm2 --without-zlib --without-brotli --without-zstd \
            --disable-doc --disable-tests --disable-tools --disable-cxx --disable-libdane \
            --disable-nls --disable-guile --disable-gcc-warnings --disable-hardware-acceleration \
            NETTLE_CFLAGS="-I$out/include" NETTLE_LIBS="-L$out/lib -lnettle" \
            HOGWEED_CFLAGS="-I$out/include" HOGWEED_LIBS="-L$out/lib -lhogweed" \
            GMP_CFLAGS="-I$out/include" GMP_LIBS="-L$out/lib -lhogweed" > configure.log 2>&1 &&
        # Only the library and the gnulib it links: the tools' gnulib (src/gl)
        # is in the tree's SUBDIRS even with the tools disabled, and its
        # pthread.h declares pthread_mutexattr_getrobust, which the SDK's
        # pthread.h declares differently and its libraries do not export.
        make -j"$jobs" -C gl > make.log 2>&1 && make -j"$jobs" -C lib >> make.log 2>&1 &&
        make -C lib install > install.log 2>&1) ||
        fail "GnuTLS did not build; see $work/gnutls-$GNUTLS_VERSION/*.log"
    echo "built GnuTLS $GNUTLS_VERSION"
fi
# The licence texts, from the sources that were built (extracted again if
# the trees are gone): GnuTLS's LGPL-2.1 and GPL-3 texts and authors, with
# its included inih's notice; nettle's LGPL-3, GPL-2 and GPL-3 texts and
# authors. libtasn1 and libunistring, included in GnuTLS, are covered by
# those texts (THIRD_PARTY.md).
licenses() {
    [ -d "$work/$1" ] || tar -xf "$work/$1.tar.$3" -C "$work" || fail "cannot extract $1"
    rm -rf "$out/licenses/$2"
    mkdir -p "$out/licenses/$2"
    for file in $4; do
        cp "$work/$1/$file" "$out/licenses/$2/$(echo "$file" | tr / -)" || fail "no $file in $1"
    done
}
if [ ! -d "$out/licenses/gnutls" ]; then
    licenses "gnutls-$GNUTLS_VERSION" gnutls xz "COPYING.LESSERv2 COPYING AUTHORS lib/inih/LICENSE.txt"
fi
if [ ! -d "$out/licenses/nettle" ]; then
    licenses "nettle-$NETTLE_VERSION" nettle gz "COPYING.LESSERv3 COPYINGv2 COPYINGv3 AUTHORS"
fi
# The bundle, as the PS5 build stages it beside the runtime.
if [ "$(sha256sum "$out/ca-certificates.crt" 2>/dev/null | cut -c1-64)" != "$CA_BUNDLE_SHA256" ]; then
    cp "$work/cacert-$CA_BUNDLE_DATE.pem" "$out/ca-certificates.crt"
    echo "root certificates: cacert-$CA_BUNDLE_DATE.pem"
fi
echo "$out"
