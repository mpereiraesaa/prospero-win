#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/build_tls_ps5.sh without a console toolchain: its pins are well
formed and agree with each other (tools/package_release.sh prints them in
SOURCES.txt), it names a missing SDK and an unknown argument, and a
download that does not match its pinned SHA-256 stops it before anything
is built."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "build_tls_ps5.sh"


def pin(name: str) -> str:
    match = re.search(rf"^{name}=(\S+)$", SCRIPT.read_text(), re.M)
    assert match, name
    return match.group(1)


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["sh", str(SCRIPT), *args], capture_output=True, text=True)


def main() -> int:
    # The pins: a version and a 64-hex SHA-256 for each tarball, the URL
    # naming the version; a dated bundle from curl's store.
    for name in ("NETTLE", "GNUTLS"):
        assert re.fullmatch(r"\d+\.\d+(\.\d+)?", pin(f"{name}_VERSION")), name
        assert re.fullmatch(r"[0-9a-f]{64}", pin(f"{name}_SHA256")), name
        url = pin(f"{name}_URL")
        assert url.startswith("https://") and f"${name}_VERSION" in url, url
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", pin("CA_BUNDLE_DATE"))
    assert re.fullmatch(r"[0-9a-f]{64}", pin("CA_BUNDLE_SHA256"))
    assert pin("CA_BUNDLE_URL") == "https://curl.se/ca/cacert-$CA_BUNDLE_DATE.pem"

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        # No SDK: named, before any download.
        bad = run("--sdk", str(root / "nowhere"), "--work", str(root / "work"))
        assert bad.returncode == 1 and "no PS5 payload SDK" in bad.stderr, bad.stderr
        assert not (root / "work").exists()
        bad = run("--frobnicate")
        assert bad.returncode == 1 and "unknown argument --frobnicate" in bad.stderr, bad.stderr
        # A fake SDK and a tarball already "downloaded" with other contents:
        # the pin check refuses it and nothing is configured or built.
        sdk = root / "sdk"
        (sdk / "bin").mkdir(parents=True)
        clang = sdk / "bin" / "prospero-clang"
        clang.write_text("#!/bin/sh\nexit 0\n")
        clang.chmod(0o755)
        work = root / "work"
        work.mkdir()
        (work / f"nettle-{pin('NETTLE_VERSION')}.tar.gz").write_bytes(b"not nettle")
        bad = run("--sdk", str(sdk), "--work", str(work))
        assert bad.returncode == 1, bad.stderr
        assert f"nettle-{pin('NETTLE_VERSION')}.tar.gz does not match its pinned SHA-256" in bad.stderr, bad.stderr
        names = sorted(p.name for p in work.iterdir())
        assert not any(name.startswith("gnutls") for name in names), names   # never reached
        assert f"nettle-{pin('NETTLE_VERSION')}" not in names, names          # not extracted
        assert not (work / "root" / "lib" / "libhogweed.a").exists()
    print("tls build script passed: pins well formed, SDK and arguments checked, "
          "a tarball off its SHA-256 refused before building")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
