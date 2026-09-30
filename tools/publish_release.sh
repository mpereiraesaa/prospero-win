#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Attach the app folder to a version's GitHub release.
#
#   tools/publish_release.sh --tag v0.1.0 --zip release/PPSA99995.zip [--publish]
#
# Pushing the tag makes .github/workflows/release.yml open a draft release;
# this uploads tools/package_release.sh's zip to it as
# prospero-win-<tag>.zip, with a SHA256SUMS file. --publish then takes the
# release out of draft. Run it again to replace the files.
set -eu

tag= zip= publish=0
fail() { echo "publish_release: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case $1 in
    --tag) tag=$2; shift 2 ;;
    --zip) zip=$2; shift 2 ;;
    --publish) publish=1; shift ;;
    *) fail "unknown argument $1" ;;
    esac
done
case $tag in v[0-9]*) ;; *) fail "--tag must be a version tag such as v0.1.0" ;; esac
[ -f "$zip" ] || fail "--zip: no file '$zip'"
listing=$(unzip -Z1 "$zip") || fail "--zip: '$zip' is not a zip"
echo "$listing" | grep -q '^PPSA99995/eboot\.bin$' || fail "--zip: no PPSA99995/eboot.bin in '$zip'"
# The builder's log destination and Windows programs never ship.
if echo "$listing" | grep -Eq '(^|/)dev\.conf$|\.exe$'; then
    fail "--zip: '$zip' holds a dev.conf or an .exe; rebuild it with tools/package_release.sh"
fi
gh release view "$tag" >/dev/null 2>&1 || fail "no release $tag yet: push the tag and wait for the Release workflow"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
asset=prospero-win-$tag.zip
cp "$zip" "$work/$asset"
( cd "$work" && sha256sum "$asset" > SHA256SUMS )
gh release upload "$tag" "$work/$asset" "$work/SHA256SUMS" --clobber
echo "publish_release: $tag: $(cat "$work/SHA256SUMS")"
if [ "$publish" = 1 ]; then
    gh release edit "$tag" --draft=false
    echo "publish_release: $tag published"
fi
