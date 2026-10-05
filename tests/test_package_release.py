#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/package_release.sh on fake inputs: the app folder's layout, what is
left out (the builder's dev.conf, import libraries, PC-only drivers) and what
the PS5 build overrides."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "package_release.sh"


def write(path: Path, data: str = "x") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data)


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["sh", str(SCRIPT), *args], capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        title, ps5, host = root / "title", root / "wine-ps5", root / "usr"
        write(title / "eboot.bin", "eboot")
        write(title / "lapy.elf", "helper")
        write(title / "sce_sys" / "param.json", "{}")
        write(title / "sce_module" / "libc.prx", "libc")
        write(title / "dev.conf", "DEV_SERVER=builder-pc")
        for name in ("ntdll.prx", "win32u.prx", "libvulkan.prx"):
            write(ps5 / "prx" / "sce_module" / name, name)
        write(ps5 / "prx" / "fonts" / "tahoma.ttf", "font")
        write(ps5 / "pe" / "i386-windows" / "xinput1_3.dll", "patched")
        for arch in ("i386-windows", "x86_64-windows"):
            for name in ("kernel32.dll", "xinput1_3.dll", "libkernel32.a", "winex11.drv",
                         "winegstreamer.dll", "wpcap.dll", "sane.ds", "gphoto2.ds", "quartz.dll"):
                write(host / "lib" / "wine" / arch / name, f"{arch}/{name}")
        write(host / "share" / "wine" / "nls" / "locale.nls", "nls")
        write(root / "wowprospero.dll", "cpu")
        out = root / "out"

        result = run("--title", str(title), "--wine-ps5", str(ps5), "--host-wine", str(host),
                     "--cpu-dll", str(root / "wowprospero.dll"), "--out", str(out))
        assert result.returncode == 0, result.stderr
        app = out / "PPSA99995"
        lib = app / "win" / "wine" / "lib" / "wine"
        share = app / "win" / "wine" / "share" / "wine"
        # The title, without the builder's log destination.
        assert (app / "eboot.bin").read_text() == "eboot"
        assert (app / "lapy.elf").read_text() == "helper"
        assert (app / "sce_sys" / "param.json").exists() and (app / "sce_module" / "libc.prx").exists()
        assert not (app / "dev.conf").exists()
        # Wine's PE modules, less import libraries and PC-only drivers; the
        # patched xinput over the host's, only where the PS5 build has one.
        for arch in ("i386-windows", "x86_64-windows"):
            names = sorted(p.name for p in (lib / arch).iterdir())
            expected = ["kernel32.dll", "quartz.dll", "xinput1_3.dll"]
            if arch == "x86_64-windows":
                expected = sorted(expected + ["wowprospero.dll"])
            assert names == expected, (arch, names)
        assert (lib / "i386-windows" / "xinput1_3.dll").read_text() == "patched"
        assert (lib / "x86_64-windows" / "xinput1_3.dll").read_text() == "x86_64-windows/xinput1_3.dll"
        assert (lib / "x86_64-windows" / "wowprospero.dll").read_text() == "cpu"
        assert sorted(p.name for p in (lib / "x86_64-unix").iterdir()) == \
            ["libvulkan.prx", "ntdll.prx", "win32u.prx"]
        assert (share / "nls" / "locale.nls").exists() and (share / "fonts" / "tahoma.ttf").exists()
        assert "PPSA99995: 16 files" in result.stdout, result.stdout

        # A second run replaces the folder rather than merging into it.
        write(app / "stale.txt")
        assert run("--title", str(title), "--wine-ps5", str(ps5), "--host-wine", str(host),
                   "--cpu-dll", str(root / "wowprospero.dll"), "--out", str(out)).returncode == 0
        assert not (app / "stale.txt").exists()

        # Missing inputs are named, not guessed.
        bad = run("--title", str(root / "nowhere"), "--wine-ps5", str(ps5), "--host-wine", str(host),
                  "--cpu-dll", str(root / "wowprospero.dll"), "--out", str(out))
        assert bad.returncode == 2 and "--title" in bad.stderr
        bad = run("--title", str(title), "--wine-ps5", str(ps5), "--host-wine", str(host),
                  "--cpu-dll", str(root / "missing.dll"), "--out", str(out))
        assert bad.returncode == 2 and "--cpu-dll" in bad.stderr
    print("package release passed: layout, dev.conf and PC-only modules left out, patched xinput, "
          "a clean folder each time, missing inputs named")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
