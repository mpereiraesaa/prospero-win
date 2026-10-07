#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/package_release.sh on fake inputs: the app folder's layout, what is
left out (the builder's dev.conf, import libraries, PC-only drivers), what
the PS5 build overrides, and the licences and source revisions it carries."""

from pathlib import Path
import json
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
        native = root / "wow64native"
        write(native / "x86_64-windows" / "wow64native.dll", "native cpu")
        write(native / "ps5" / "wow64native.prx", "native prx")
        # What the licences and SOURCES.txt come from: the build's report,
        # the Wine and FreeType sources it built, the helper's release.
        report = {"wine_commit": "a" * 40, "patches": ["0100-x.patch", "0110-y.patch"],
                  "sources": {"prx_foundation": "b" * 40, "ps5_mesa": "c" * 40, "ps5_vulkan": "d" * 40,
                              "radv_payload_sdk": "9" * 40, "ps5vk": None, "ps5_opengl_sdk": None,
                              "ps5_opengl": None}}
        write(ps5 / "report.json", json.dumps(report))
        for name in ("LICENSE", "COPYING.LIB", "AUTHORS", "NOTICES.md"):
            write(ps5 / "source" / name, f"wine {name}")
        write(ps5 / "source" / "libs" / "faudio" / "LICENSE", "faudio licence")
        write(ps5 / "source" / "libs" / "ldap" / "COPYRIGHT", "ldap copyright")
        write(ps5 / "source" / "libs" / "uuid" / "uuid.c", "no licence file")
        write(ps5 / "freetype" / "src" / "LICENSE.TXT", "freetype licences")
        write(ps5 / "freetype" / "src" / "docs" / "FTL.TXT", "FTL")
        lapy = root / "lapy-helper-release.json"
        write(lapy, json.dumps({"tag_name": "v9.9.9", "release_url": "https://example.invalid/v9.9.9"}))
        out = root / "out"
        inputs = ("--title", str(title), "--wine-ps5", str(ps5), "--host-wine", str(host),
                  "--cpu-dll", str(root / "wowprospero.dll"), "--native-cpu", str(native),
                  "--lapy-release", str(lapy))

        result = run(*inputs, "--out", str(out))
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
                expected = sorted(expected + ["wow64native.dll", "wowprospero.dll"])
            assert names == expected, (arch, names)
        assert (lib / "i386-windows" / "xinput1_3.dll").read_text() == "patched"
        assert (lib / "x86_64-windows" / "xinput1_3.dll").read_text() == "x86_64-windows/xinput1_3.dll"
        assert (lib / "x86_64-windows" / "wowprospero.dll").read_text() == "cpu"
        assert (lib / "x86_64-windows" / "wow64native.dll").read_text() == "native cpu"
        assert (lib / "x86_64-unix" / "wow64native.prx").read_text() == "native prx"
        assert sorted(p.name for p in (lib / "x86_64-unix").iterdir()) == \
            ["libvulkan.prx", "ntdll.prx", "win32u.prx", "wow64native.prx"]
        assert (share / "nls" / "locale.nls").exists() and (share / "fonts" / "tahoma.ttf").exists()
        assert "PPSA99995: 33 files" in result.stdout, result.stdout
        # The licences: the project's own texts verbatim, Wine's and its
        # libraries' from the built source, FreeType's.
        for name in ("LICENSE", "THIRD_PARTY.md"):
            assert (app / name).read_bytes() == (ROOT / name).read_bytes(), name
        committed = sorted(p.name for p in (ROOT / "LICENSES").iterdir())
        assert committed == ["Apache-2.0-WITH-LLVM-exception.txt", "GPL-3.0.txt", "Lapy-MIT.txt",
                             "Mesa-MIT.txt"], committed
        for name in committed:
            assert (app / "LICENSES" / name).read_bytes() == (ROOT / "LICENSES" / name).read_bytes(), name
        licences = app / "LICENSES"
        assert sorted(p.name for p in (licences / "wine").iterdir()) == \
            ["AUTHORS", "COPYING.LIB", "LICENSE", "NOTICES.md", "libs"]
        assert (licences / "wine" / "NOTICES.md").read_text() == "wine NOTICES.md"
        assert sorted(p.name for p in (licences / "wine" / "libs").iterdir()) == ["faudio", "ldap"]
        assert (licences / "wine" / "libs" / "faudio" / "LICENSE").read_text() == "faudio licence"
        assert (licences / "wine" / "libs" / "ldap" / "COPYRIGHT").read_text() == "ldap copyright"
        assert sorted(p.name for p in (licences / "freetype").iterdir()) == ["FTL.TXT", "LICENSE.TXT"]
        # SOURCES.txt: every revision the build recorded, and no OpenGL.
        sources = (app / "SOURCES.txt").read_text()
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        for expected in (f"prospero-win  https://github.com/mpereiraesaa/prospero-win  commit {head}",
                         f"Wine  https://gitlab.winehq.org/wine/wine  commit {'a' * 40}",
                         "wine/patches (2 patches)", f"PS5 module tools: commit {'b' * 40}",
                         "title: commit 9c0b994a048521af6fb84c73ded364504fe250e9",
                         "FreeType 2.13.3  https://download.savannah.gnu.org/releases/freetype/"
                         "freetype-2.13.3.tar.xz",
                         "SHA-256 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289",
                         "https://example.invalid/v9.9.9  release v9.9.9",
                         f"PS5_Mesa  commit {'c' * 40}", f"PS5_Vulkan  commit {'d' * 40}",
                         f"PS5_PayloadSDK  commit {'9' * 40}",
                         "OpenGL  not included"):
            assert expected in sources, (expected, sources)
        # The console refuses to exec an eboot or load a PRX without execute
        # permission, whatever mode the inputs had; data files stay as they were.
        for name in ("eboot.bin", "sce_module/libc.prx", "win/wine/lib/wine/x86_64-unix/ntdll.prx",
                     "win/wine/lib/wine/x86_64-unix/libvulkan.prx",
                     "win/wine/lib/wine/x86_64-unix/wow64native.prx"):
            assert (app / name).stat().st_mode & 0o777 == 0o755, name
        assert not (app / "sce_sys" / "param.json").stat().st_mode & 0o111
        assert not (lib / "x86_64-windows" / "wowprospero.dll").stat().st_mode & 0o111

        # A second run replaces the folder rather than merging into it.
        write(app / "stale.txt")
        assert run(*inputs, "--out", str(out)).returncode == 0
        assert not (app / "stale.txt").exists()

        # An OpenGL build says so, with the SDK it linked.
        report["sources"].update(ps5_opengl_sdk="e" * 64, ps5_opengl="8" * 40)
        write(ps5 / "report.json", json.dumps(report))
        assert run(*inputs, "--out", str(out)).returncode == 0
        sources = (app / "SOURCES.txt").read_text()
        assert f"OpenGL (in win32u.prx)  https://github.com/mpereiraesaa/ps5-opengl  commit {'8' * 40}" \
            in sources, sources
        assert f"SDK manifest SHA-256 {'e' * 64}" in sources and "not included" not in sources
        # A libvulkan.prx that is not RADV has no notice here: refused.
        report["sources"].update(ps5_mesa=None, ps5_vulkan=None, ps5vk="f" * 64)
        write(ps5 / "report.json", json.dumps(report))
        bad = run(*inputs, "--out", str(out))
        assert bad.returncode == 2 and "RADV" in bad.stderr, bad.stderr

        # Missing inputs are named, not guessed.
        bad = run("--title", str(root / "nowhere"), "--wine-ps5", str(ps5), "--host-wine", str(host),
                  "--cpu-dll", str(root / "wowprospero.dll"), "--out", str(out))
        assert bad.returncode == 2 and "--title" in bad.stderr
        bad = run("--title", str(title), "--wine-ps5", str(ps5), "--host-wine", str(host),
                  "--cpu-dll", str(root / "missing.dll"), "--lapy-release", str(lapy), "--out", str(out))
        assert bad.returncode == 2 and "--cpu-dll" in bad.stderr
        bad = run(*inputs[:-2], "--out", str(out))
        assert bad.returncode == 2 and "--lapy-release" in bad.stderr, bad.stderr
        (native / "ps5" / "wow64native.prx").unlink()
        bad = run(*inputs, "--out", str(out))
        assert bad.returncode == 2 and "--native-cpu" in bad.stderr, bad.stderr
        write(native / "ps5" / "wow64native.prx", "native prx")
        (ps5 / "source" / "NOTICES.md").unlink()
        bad = run(*inputs, "--out", str(out))
        assert bad.returncode == 2 and "NOTICES.md" in bad.stderr, bad.stderr
    print("package release passed: layout, dev.conf and PC-only modules left out, patched xinput, "
          "eboot and PRX modules executable, licences and source revisions, OpenGL noted, "
          "non-RADV Vulkan refused, a clean folder each time, missing inputs named")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
