#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/publish_release.sh with a fake gh: what it refuses to upload, the
asset's name and checksum, and when it publishes the draft."""

from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "publish_release.sh"

# Logs each call; `gh release view` fails unless the release exists.
FAKE_GH = """#!/bin/sh
echo "$*" >> "$GH_LOG"
if [ "$1 $2" = "release view" ] && [ ! -e "$GH_HAS_RELEASE" ]; then exit 1; fi
if [ "$1 $2" = "release upload" ]; then cp "$4" "$5" "$GH_UPLOADS/"; fi
exit 0
"""


def make_zip(path: Path, *names: str) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for name in names:
            archive.writestr(name, name)


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        bin_dir, uploads = root / "bin", root / "uploads"
        bin_dir.mkdir()
        uploads.mkdir()
        (bin_dir / "gh").write_text(FAKE_GH)
        (bin_dir / "gh").chmod(0o755)
        log, has_release = root / "gh.log", root / "has-release"
        env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}", GH_LOG=str(log),
                   GH_HAS_RELEASE=str(has_release), GH_UPLOADS=str(uploads))

        def run(*args: str) -> subprocess.CompletedProcess:
            return subprocess.run(["sh", str(SCRIPT), *args], capture_output=True, text=True, env=env)

        good = root / "good.zip"
        # Wine's own programs ship beside its DLLs; any other .exe is a game's.
        make_zip(good, "PPSA99995/eboot.bin", "PPSA99995/win/wine/lib/wine/x86_64-unix/ntdll.prx",
                 "PPSA99995/win/wine/lib/wine/i386-windows/wineboot.exe",
                 "PPSA99995/win/wine/lib/wine/x86_64-windows/rundll32.exe")
        with_conf = root / "conf.zip"
        make_zip(with_conf, "PPSA99995/eboot.bin", "PPSA99995/dev.conf")
        with_exe = root / "exe.zip"
        make_zip(with_exe, "PPSA99995/eboot.bin", "PPSA99995/game/war3.exe")
        nested_exe = root / "nested-exe.zip"
        make_zip(nested_exe, "PPSA99995/eboot.bin", "PPSA99995/win/wine/lib/wine/i386-windows/game/war3.exe")
        no_title = root / "empty.zip"
        make_zip(no_title, "other/eboot.bin")

        refusals = [
            (("--tag", "latest", "--zip", str(good)), "version tag"),
            (("--tag", "v0.1.0", "--zip", str(root / "missing.zip")), "no file"),
            (("--tag", "v0.1.0", "--zip", str(no_title)), "no PPSA99995/eboot.bin"),
            (("--tag", "v0.1.0", "--zip", str(with_conf)), "dev.conf or an .exe"),
            (("--tag", "v0.1.0", "--zip", str(with_exe)), "dev.conf or an .exe"),
            (("--tag", "v0.1.0", "--zip", str(nested_exe)), "dev.conf or an .exe"),
            (("--tag", "v0.1.0", "--zip", str(good)), "no release v0.1.0"),
            (("--tag", "v0.1.0", "--zip", str(good), "--force"), "unknown argument --force"),
        ]
        for args, message in refusals:
            result = run(*args)
            assert result.returncode == 2, (args, result)
            assert message in result.stderr, (args, result.stderr)
        assert not any(uploads.iterdir()), "nothing is uploaded before every check passes"

        has_release.touch()
        log.unlink()
        result = run("--tag", "v0.1.0", "--zip", str(good))
        assert result.returncode == 0, result.stderr
        asset = uploads / "prospero-win-v0.1.0.zip"
        assert asset.read_bytes() == good.read_bytes()
        digest = hashlib.sha256(good.read_bytes()).hexdigest()
        assert (uploads / "SHA256SUMS").read_text() == f"{digest}  prospero-win-v0.1.0.zip\n"
        calls = log.read_text().splitlines()
        assert calls[0] == "release view v0.1.0", calls
        assert calls[1].startswith("release upload v0.1.0 ") and calls[1].endswith(" --clobber"), calls
        assert len(calls) == 2, "a draft stays a draft without --publish"

        log.unlink()
        result = run("--tag", "v0.1.0", "--zip", str(good), "--publish")
        assert result.returncode == 0, result.stderr
        assert log.read_text().splitlines()[-1] == "release edit v0.1.0 --draft=false"
    print("test_publish_release: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
