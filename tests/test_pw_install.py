#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/pw_install.py runs Lutris installer scripts with a given Wine.

A fake Wine records each call and does what the real one would to the
prefix: wineboot writes the registry files, regedit appends its .reg, and an
installer .exe runs the shell script beside it."""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pw_install  # noqa: E402

FAKE_WINE = r"""#!/bin/sh
echo "wine $* | prefix=$WINEPREFIX | overrides=$WINEDLLOVERRIDES | user=$USER" >> "$FAKE_LOG"
case "$1" in
wineboot)
    mkdir -p "$WINEPREFIX/drive_c/windows/system32" "$WINEPREFIX/drive_c/windows/syswow64"
    echo WINE REGISTRY > "$WINEPREFIX/system.reg"; echo WINE REGISTRY > "$WINEPREFIX/user.reg"
    # As the real one does: the user's folders link to the PC's home.
    mkdir -p "$WINEPREFIX/drive_c/users/$USER" "$WINEPREFIX/dosdevices"
    ln -sfn ../drive_c "$WINEPREFIX/dosdevices/c:"
    ln -sfn "$FAKE_HOME" "$WINEPREFIX/drive_c/users/$USER/Desktop"
    ln -sfn "$FAKE_HOME/Documents" "$WINEPREFIX/drive_c/users/$USER/Documents" ;;
regedit) cat "$3" >> "$WINEPREFIX/user.reg" ;;
*) [ -f "$1.sh" ] && sh "$1.sh" "$@"; exit "${FAKE_EXIT:-0}" ;;
esac
"""


def pe(bits: int) -> bytes:
    """The smallest header pw_install reads: MZ, e_lfanew, PE, magic."""
    data = bytearray(0x100)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x80 + 24, 0x10B if bits == 32 else 0x20B)
    return bytes(data)



def mesa_pe(bits: int, dependency: str | None = None) -> bytes:
    data = bytearray(1024); data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 0x80); data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x14c if bits == 32 else 0x8664)
    struct.pack_into("<H", data, 0x94, 240)
    optional = 0x98; directory = 96 if bits == 32 else 112
    struct.pack_into("<H", data, optional, 0x10b if bits == 32 else 0x20b)
    struct.pack_into("<I", data, optional + 60, len(data))
    struct.pack_into("<I", data, optional + directory - 4, 16)
    if dependency:
        struct.pack_into("<II", data, optional + directory + 8, 0x200, 40)
        struct.pack_into("<I", data, 0x20c, 0x260)
        data[0x260:0x260 + len(dependency)] = dependency.encode()
    return bytes(data)


def fake_mesa(root: Path, runtime: bool = True) -> Path:
    root.mkdir(); records = {}
    for arch, bits in (("i386-windows", 32), ("x86_64-windows", 64)):
        (root / arch).mkdir()
        for name, dependency in (("opengl32.dll", "libgallium_wgl.dll"), ("libgallium_wgl.dll", "libc++.dll" if runtime else None), ("libc++.dll", None)):
            data = mesa_pe(bits, dependency); path = root / arch / name; path.write_bytes(data)
            records[arch + "/" + name] = {"sha256": hashlib.sha256(data).hexdigest(), "machine": 0x14c if bits == 32 else 0x8664, "imports": [dependency] if dependency else []}
    (root / "manifest.json").write_text(json.dumps({"driver":"zink", "architectures":["i386-windows","x86_64-windows"], "mesa_commit":"1" * 40, "llvm_mingw_archive_sha256":"2" * 64, "files":records}))
    return root


def mesa_rehash(root: Path, relative: str) -> None:
    manifest = json.loads((root / "manifest.json").read_text())
    manifest["files"][relative]["sha256"] = hashlib.sha256((root / relative).read_bytes()).hexdigest()
    (root / "manifest.json").write_text(json.dumps(manifest))


def check_mesa() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        package = fake_mesa(root / "valid")
        result = pw_install.mesa_provider(package)
        assert set(result) == {"i386-windows", "x86_64-windows"}
        assert all(len(x) == 3 for x in result.values())
        # Section-backed PE32/PE64 import RVAs are mapped by raw extent.
        for bits, machine in ((32,0x14c),(64,0x8664)):
            path = root / f"section-{bits}.dll"
            data = bytearray(mesa_pe(bits,"libgallium_wgl.dll"));directory=96 if bits==32 else 112
            struct.pack_into("<H",data,0x86,1)
            struct.pack_into("<I",data,0x98+60,0x200)
            struct.pack_into("<II",data,0x98+directory+8,0x1000,40)
            struct.pack_into("<III",data,0x188+12,0x1000,0x200,0x200)
            struct.pack_into("<I",data,0x20c,0x1060);path.write_bytes(data)
            assert pw_install.mesa_pe(path,machine)==["libgallium_wgl.dll"]
            struct.pack_into("<I",data,0x20c,0x1300);path.write_bytes(data)
            try: pw_install.mesa_pe(path,machine)
            except pw_install.InstallError:pass
            else:raise AssertionError("Unreadable section import accepted")
        # Python IGNORECASE must not admit Unicode outliers rejected by native names.
        for index, character in enumerate(("K", "İ", "ı", "ſ")):
            package=fake_mesa(root / f"unicode-{index}")
            relative="i386-windows/"+character+"compiler.dll";data=mesa_pe(32);(package/relative).write_bytes(data)
            manifest=json.loads((package/"manifest.json").read_text())
            manifest["files"][relative]={"sha256":hashlib.sha256(data).hexdigest(),"machine":0x14c,"imports":[]}
            (package/"manifest.json").write_text(json.dumps(manifest))
            try:pw_install.mesa_provider(package)
            except pw_install.InstallError:pass
            else:raise AssertionError("Unicode compiler name accepted")
        for kind in ("hash", "machine", "missing", "runtime", "symlink", "unsafe", "unmanifested", "case-extra", "long-name", "imports", "name", "descriptors", "count"):
            package = fake_mesa(root / kind)
            path = package / "i386-windows/opengl32.dll"
            manifest = json.loads((package / "manifest.json").read_text())
            if kind == "hash": path.write_bytes(path.read_bytes() + b"changed")
            elif kind == "machine":
                data = bytearray(path.read_bytes()); struct.pack_into("<H",data,0x84,0x8664);path.write_bytes(data);mesa_rehash(package,"i386-windows/opengl32.dll")
            elif kind in ("missing", "runtime"):
                name = "libgallium_wgl.dll" if kind == "missing" else "libc++.dll"
                (package / "i386-windows" / name).unlink(); del manifest["files"]["i386-windows/" + name]
            elif kind == "symlink": path.unlink();path.symlink_to(package / "x86_64-windows/opengl32.dll")
            elif kind == "unsafe": manifest["files"]["i386-windows/../escape.dll"] = manifest["files"].pop("i386-windows/opengl32.dll")
            elif kind == "unmanifested": (package / "i386-windows/extra.dll").write_bytes(mesa_pe(32))
            elif kind == "case-extra": (package / "i386-windows/OPENGL32.dll").write_bytes(mesa_pe(32))
            elif kind == "long-name":
                relative="i386-windows/"+"a"*124+".dll";data=mesa_pe(32);(package/relative).write_bytes(data)
                manifest["files"][relative]={"sha256":hashlib.sha256(data).hexdigest(),"machine":0x14c,"imports":[]}
            elif kind == "imports": manifest["files"]["i386-windows/opengl32.dll"]["imports"] = []
            elif kind in ("name", "descriptors"):
                data = bytearray(path.read_bytes())
                if kind == "name": data[0x260:0x260+128] = b"x" * 128
                else: struct.pack_into("<I",data,0x98+96+12,20)
                path.write_bytes(data);mesa_rehash(package,"i386-windows/opengl32.dll")
            elif kind == "count":
                for index in range(30):
                    name = f"extra{index}.dll";data=mesa_pe(32);(package / "i386-windows" / name).write_bytes(data)
                    manifest["files"]["i386-windows/"+name]={"sha256":hashlib.sha256(data).hexdigest(),"machine":0x14c,"imports":[]}
            if kind not in ("machine", "name", "descriptors"):
                (package / "manifest.json").write_text(json.dumps(manifest))
            try: pw_install.mesa_provider(package)
            except pw_install.InstallError: pass
            else: raise AssertionError("Accepted bad Mesa package: " + kind)

def fake_dxvk(root: Path) -> tuple[Path, str]:
    archive = root / "dxvk.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for bits in ("x32", "x64"):
            for dll in pw_install.DXVK_DLLS:
                payload = f"{bits}/{dll}".encode()
                info = tarfile.TarInfo(f"dxvk-9.9/{bits}/{dll}.dll")
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
    return archive, hashlib.sha256(archive.read_bytes()).hexdigest()


SCRIPT = r"""
name: Test Game
game_slug: test-game
runner: wine
prospero:
  display: {desktop: 1920x1080, scaling: fit, show_fps: true, refresh: 120, opengl_thread: true}
  input: {preset: mouse}
script:
  game:
    exe: drive_c/Games/Test/game.exe
    args: -window -opengl
    prefix: $GAMEDIR
  files:
  - setup: "N/A:Select the game's setup file"
  wine:
    dxvk: true
    dxvk_version: "9.9"
    overrides: {ddraw.dll: n}
  installer:
  - task: {name: create_prefix, prefix: $GAMEDIR, arch: win32}
  - input_menu:
      id: LANG
      description: Language
      options: [{en: English}, {es: Spanish}]
      preselect: en
  - task: {name: wineexec, executable: setup, args: /S /LANG=$INPUT_LANG, return_code: "0,25856"}
  - task: {name: set_regedit, path: 'HKEY_CURRENT_USER\Software\Test\Video', key: reswidth,
           value: $RESOLUTION_WIDTH, type: REG_DWORD}
  - task: {name: set_regedit, path: 'HKEY_CURRENT_USER\Software\Test', key: InstallPath, value: 'C:\Games\Test'}
  - write_config:
      file: $GAMEDIR/drive_c/Games/Test/game.ini
      section: Video
      key: Height
      value: $RESOLUTION_HEIGHT
"""


def check_dxvk_releases() -> None:
    """Upstream releases and custom builds from the fork, pinned by hash."""
    url, digest = pw_install.dxvk_release("2.6.2")
    assert url == "https://github.com/doitsujin/dxvk/releases/download/v2.6.2/dxvk-2.6.2.tar.gz"
    assert digest == "17761876556afd55736cb895d184f5a1c55d43350f1b1e3b129f8d28706d7992"
    url, digest = pw_install.dxvk_release("2.6.2-prospero1")
    assert url == ("https://github.com/mpereiraesaa/dxvk/releases/download/"
                   "v2.6.2-prospero1/dxvk-2.6.2-prospero1.tar.gz")
    assert digest == "72a4d7e279f522ad9bac420caf13665bcdd28b297337e1364c31c5bda24ef3f2"
    url, digest = pw_install.dxvk_release("2.6.2-prospero2")
    assert url == ("https://github.com/mpereiraesaa/dxvk/releases/download/"
                   "v2.6.2-prospero2/dxvk-2.6.2-prospero2.tar.gz")
    assert digest == "c00c1cd7df7ce70e89130b61c1fcd2d3a18ac473f274a219da4478147e4f3c90"
    # install_dxvk reads dxvk-<version>/x32 and x64 from an archive named
    # after the release, upstream's layout, which every pinned one follows.
    for version, (url, digest) in pw_install.DXVK_RELEASES.items():
        owner = url.split("/")[3]
        assert owner in ("doitsujin", "mpereiraesaa"), url
        assert (owner == "mpereiraesaa") == ("-prospero" in version), version
        assert url.endswith(f"/releases/download/v{version}/dxvk-{version}.tar.gz"), url
        assert re.fullmatch(r"[0-9a-f]{64}", digest), version
    for version in ("2.6.2-prospero0", "2.6", ""):
        try:
            pw_install.dxvk_release(version)
        except pw_install.InstallError as error:
            assert f"dxvk_version {version} is not pinned" in str(error)
            assert "'2.6.2-prospero1'" in str(error) and "'2.6.2-prospero2'" in str(error)
        else:
            raise AssertionError(f"dxvk_version {version!r} should not resolve")


def main() -> int:
    check_dxvk_releases()
    check_mesa()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        wine = root / "wine" / "wine"
        wine.parent.mkdir()
        wine.write_text(FAKE_WINE)
        wine.chmod(0o755)
        (root / "wine" / "wineserver").write_text("#!/bin/sh\nexit 0\n")
        (root / "wine" / "wineserver").chmod(0o755)
        log = root / "calls.log"
        os.environ["FAKE_LOG"] = str(log)
        home = root / "home"
        (home / "Documents").mkdir(parents=True)
        (home / "Documents/mine.txt").write_text("the PC user's")
        os.environ["FAKE_HOME"] = str(home)
        setup = root / "setup.exe"
        setup.write_bytes(pe(32))
        # The "installer" writes the game into the prefix, as a real one would.
        (root / "setup.exe.sh").write_text(
            'mkdir -p "$WINEPREFIX/drive_c/Games/Test"\n'
            f'cp "{setup}" "$WINEPREFIX/drive_c/Games/Test/game.exe"\n'
            'echo "$@" > "$WINEPREFIX/installer-args"\n')
        archive, digest = fake_dxvk(root)
        pw_install.DXVK_RELEASES["9.9"] = ("file://unused", digest)
        pw_install.DOWNLOADS = root / "downloads"
        pw_install.DOWNLOADS.mkdir()
        os.link(archive, pw_install.DOWNLOADS / "dxvk-9.9.tar.gz")
        script = root / "test.yml"
        script.write_text(SCRIPT)
        library = root / "library"
        common = [str(script), "--library", str(library), "--wine", str(wine)]

        # A missing N/A: file names the file and how to give it.
        assert pw_install.main(common) == 1

        assert pw_install.main(common + ["--file", f"setup={setup}", "--input", "LANG=es",
                                         "--resolution", "2560x1440"]) == 0
        prefix = library / "prefixes" / "test-game"
        calls = log.read_text()
        assert f"prefix={prefix}" in calls
        assert "wine wineboot --init" in calls and ";mshtml=" in calls and ";mscoree=" in calls
        assert "ddraw=n" in calls and "winemenubuilder.exe=d" in calls
        assert "user=prospero" in calls
        # The user's folders are real, empty folders in the prefix, as on the
        # console; links inside the prefix stay, and the PC's home is untouched.
        for folder in ("Desktop", "Documents"):
            user_folder = prefix / "drive_c/users/prospero" / folder
            assert user_folder.is_dir() and not user_folder.is_symlink() and not any(user_folder.iterdir())
        assert os.readlink(prefix / "dosdevices/c:") == "../drive_c"
        assert sorted(path.name for path in home.rglob("*")) == ["Documents", "mine.txt"]
        assert (prefix / "installer-args").read_text().split()[1:] == ["/S", "/LANG=es"]
        registry = (prefix / "user.reg").read_text()
        assert '"reswidth"=dword:00000a00' in registry
        assert '"InstallPath"="C:\\\\Games\\\\Test"' in registry
        assert '"d3d8"="native"' in registry and '"dxgi"="native"' in registry
        assert (prefix / "drive_c/windows/syswow64/d3d8.dll").read_bytes() == b"x32/d3d8"
        assert (prefix / "drive_c/windows/system32/d3d11.dll").read_bytes() == b"x64/d3d11"
        assert "Height=1440" in (prefix / "drive_c/Games/Test/game.ini").read_text()
        assert not (library / ".cache" / "test-game").exists()
        profile = (library / "profiles" / "test-game.profile").read_text()
        for line in ("id = test-game", "name = Test Game", "executable = C:\\Games\\Test\\game.exe",
                     "working_directory = C:\\Games\\Test", "arguments = -window -opengl",
                     "dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi,ddraw=n", "prefix = test-game",
                     "runtime = wine-wow64", "architecture = pe32", "graphics = dxvk",
                     "[display]", "desktop = 1920x1080", "scaling = fit", "show_fps = true", "refresh = 120", "opengl_thread = true", "[input]", "preset = mouse"):
            assert line in profile.splitlines(), (line, profile)

        # A PS5 backend choice overrides Lutris's DXVK setting for this game.
        # OpenGL draws through Zink, so it needs the Mesa package.
        mesa = fake_mesa(root / "mesa")
        opengl_script = root / "opengl.yml"
        opengl_script.write_text(SCRIPT.replace("  display:", "  graphics: opengl\n  display:", 1)
                                 .replace("overrides: {ddraw.dll: n}",
                                          "overrides: {ddraw.dll: n, opengl32.dll: n}"))
        opengl_args = [str(opengl_script), "--library", str(library), "--wine", str(wine),
                       "--file", f"setup={setup}"]
        assert pw_install.main(opengl_args + ["--slug", "opengl-no-mesa"]) == 1
        assert not (library / "prefixes" / "opengl-no-mesa").exists()
        assert pw_install.main(opengl_args + ["--slug", "opengl-game", "--mesa-zink", str(mesa)]) == 0
        opengl_prefix = library / "prefixes" / "opengl-game"
        opengl_profile = (library / "profiles" / "opengl-game.profile").read_text()
        assert "graphics = opengl" in opengl_profile.splitlines()
        assert "d3d8=n" not in opengl_profile
        assert "opengl32=n" in opengl_profile
        assert "opengl32=b" not in opengl_profile
        assert (opengl_prefix / "drive_c/windows/syswow64/opengl32.dll").read_bytes() == \
            (mesa / "i386-windows/opengl32.dll").read_bytes()
        assert not (opengl_prefix / "drive_c/windows/syswow64/d3d8.dll").exists()
        assert '"d3d8"="native"' not in (opengl_prefix / "user.reg").read_text()

        dxvk_script = root / "forced-dxvk.yml"
        dxvk_script.write_text(SCRIPT.replace("  display:", "  graphics: dxvk\n  display:", 1)
                               .replace("dxvk: true", "dxvk: false"))
        assert pw_install.main([str(dxvk_script), "--library", str(library), "--wine", str(wine),
                                "--slug", "forced-dxvk", "--file", f"setup={setup}"]) == 0
        forced_prefix = library / "prefixes" / "forced-dxvk"
        assert "graphics = dxvk" in (library / "profiles" / "forced-dxvk.profile").read_text()
        assert (forced_prefix / "drive_c/windows/syswow64/d3d8.dll").read_bytes() == b"x32/d3d8"

        invalid_script = root / "invalid-graphics.yml"
        invalid_script.write_text(SCRIPT.replace("  display:", "  graphics: metal\n  display:", 1))
        assert pw_install.main([str(invalid_script), "--library", str(library), "--wine", str(wine),
                                "--slug", "invalid-graphics", "--file", f"setup={setup}"]) == 1
        assert not (library / "prefixes" / "invalid-graphics").exists()

        # Explicit local Zink packages install only the game's architecture.
        zink_script = root / "zink.yml"
        zink_script.write_text(SCRIPT.replace("  display:", "  graphics: zink\n  display:", 1))
        zink_args = [str(zink_script), "--library", str(library), "--wine", str(wine), "--file", f"setup={setup}"]
        assert pw_install.main(zink_args + ["--slug", "zink-missing"]) == 1
        assert not (library / "prefixes/zink-missing").exists()
        assert pw_install.main(common + ["--slug", "unused-mesa", "--mesa-zink", str(mesa)]) == 1
        assert not (library / "prefixes/unused-mesa").exists()
        for bits, folder, other in ((32,"syswow64","system32"), (64,"system32","syswow64")):
            setup.write_bytes(pe(bits));slug = f"zink-{bits}"
            assert pw_install.main(zink_args + ["--slug",slug,"--mesa-zink",str(mesa)]) == 0
            win = library / "prefixes" / slug / "drive_c/windows"
            arch = "i386-windows" if bits == 32 else "x86_64-windows"
            for name in ("opengl32.dll","libgallium_wgl.dll","libc++.dll"):
                assert (win/folder/name).read_bytes() == (mesa/arch/name).read_bytes()
                assert not (win/other/name).exists()
            assert not (win/folder/"d3d9.dll").exists()
            profile = (library/"profiles"/(slug+".profile")).read_text()
            # zink is the older name for opengl
            assert "graphics = opengl" in profile and "opengl32=n" in profile
            assert '\"opengl32\"=\"native\"' in (library/"prefixes"/slug/"user.reg").read_text()
        setup.write_bytes(pe(32))
        # Conflicting case/grouped overrides are rejected before creating a prefix.
        conflict = root / "zink-conflict.yml"
        conflict.write_text(zink_script.read_text().replace("overrides: {ddraw.dll: n}", "overrides: {\"ddraw,OPENGL32.dll\": b}"))
        assert pw_install.main([str(conflict),"--library",str(library),"--wine",str(wine),"--slug","zink-conflict","--mesa-zink",str(mesa)]) == 1
        assert not (library/"prefixes/zink-conflict").exists()
        conflict.write_text(zink_script.read_text().replace("overrides: {ddraw.dll: n}", "overrides: {\"ddraw, OPENGL32.dll\": b}"))
        assert pw_install.main([str(conflict),"--library",str(library),"--wine",str(wine),"--slug","zink-space-conflict","--mesa-zink",str(mesa)]) == 1
        assert not (library/"prefixes/zink-space-conflict").exists()
        # A failed provider replacement never emits a usable profile; own temps removed.
        original_replace = pw_install.os.replace
        def fail_mesa_replace(source, destination):
            if str(source).endswith(".mesa-new"): raise OSError("fixture replacement failure")
            return original_replace(source,destination)
        pw_install.os.replace = fail_mesa_replace
        try:
            assert pw_install.main(zink_args+["--slug","zink-copy-failed","--mesa-zink",str(mesa)]) == 1
        finally: pw_install.os.replace = original_replace
        assert not (library/"profiles/zink-copy-failed.profile").exists()
        assert not list((library/"prefixes/zink-copy-failed").rglob("*.mesa-new"))

        original_setup = (root/"setup.exe.sh").read_text()
        (root/"setup.exe.sh").write_text(original_setup+'echo original-stale > "$WINEPREFIX/drive_c/windows/syswow64/opengl32.dll.mesa-new"\n')
        try:
            assert pw_install.main(zink_args+["--slug","zink-stale","--mesa-zink",str(mesa)]) == 1
        finally:(root/"setup.exe.sh").write_text(original_setup)
        assert (library/"prefixes/zink-stale/drive_c/windows/syswow64/opengl32.dll.mesa-new").read_text()=="original-stale\n"
        assert not (library/"profiles/zink-stale.profile").exists()

        # An installed game is never overwritten.
        assert pw_install.main(common + ["--file", f"setup={setup}"]) == 1

        # An exit code the script does not accept fails the install.
        os.environ["FAKE_EXIT"] = "3"
        assert pw_install.main(common + ["--slug", "other", "--file", f"setup={setup}"]) == 1
        del os.environ["FAKE_EXIT"]

        # Unknown directives and tasks are refused, not skipped.
        for step in ("- gogdl_setup: {game_id: 1}", "- task: {name: winecfg}",
                     "- task: {name: set_regedit, path: X, key: k, value: v, type: REG_MULTI_SZ}"):
            bad = root / "bad.yml"
            bad.write_text(f"game_slug: bad\nscript:\n  game: {{exe: x.exe}}\n  installer:\n  {step}\n")
            assert pw_install.main([str(bad), "--library", str(root / "badlib"), "--wine", str(wine)]) == 1
            shutil.rmtree(root / "badlib", ignore_errors=True)

        # Only wine scripts, and only slugs a profile id accepts.
        for text in ("game_slug: x\nrunner: dosbox\nscript: {}\n", "game_slug: Bad_Slug\nscript: {}\n"):
            bad = root / "bad.yml"
            bad.write_text(text)
            assert pw_install.main([str(bad), "--library", str(root / "badlib"), "--wine", str(wine)]) == 1

        # A Wine without a wineserver beside it is refused up front.
        (root / "lonely").mkdir()
        (root / "lonely" / "wine").write_text("")
        assert pw_install.main([str(script), "--library", str(root / "lib2"), "--wine", str(root / "lonely" / "wine")]) == 1

        # install_gecko puts Wine's pinned Gecko in its download cache and
        # leaves mshtml on; the packages are checked against their hashes.
        pw_install.WINE_CACHE = root / "wine-cache"
        pw_install.WINE_CACHE.mkdir()
        digests = {}
        for arch in ("x86", "x86_64"):
            package = pw_install.WINE_CACHE / f"wine-gecko-9.9-{arch}.msi"
            package.write_bytes(arch.encode())
            digests[arch] = hashlib.sha256(arch.encode()).hexdigest()
        pw_install.GECKO = ("9.9", digests)
        gecko = root / "gecko.yml"
        gecko.write_text("game_slug: gecko\nscript:\n  game: {exe: drive_c/x.exe}\n  installer:\n"
                         "  - task: {name: create_prefix, install_gecko: true}\n")
        log.write_text("")
        assert pw_install.main([str(gecko), "--library", str(root / "glib"), "--wine", str(wine)]) == 1  # no exe
        assert "mshtml=" not in log.read_text() and ";mscoree=" in log.read_text()
        pw_install.GECKO = ("9.9", dict(digests, x86="0" * 64))
        shutil.rmtree(root / "glib")
        assert pw_install.main([str(gecko), "--library", str(root / "glib"), "--wine", str(wine)]) == 1

        # A file pinned by sha256 is downloaded to the cache and checked; a
        # wrong hash fails the install.
        mod = root / "mod.mix"
        mod.write_bytes(pe(32))
        pinned = hashlib.sha256(mod.read_bytes()).hexdigest()
        for digest, expected in ((pinned, 0), ("0" * 64, 1)):
            for cached in pw_install.DOWNLOADS.glob("pinned-*.mix"):
                cached.unlink()
            modded = root / "modded.yml"
            modded.write_text(
                "game_slug: modded\nscript:\n  game: {exe: drive_c/Games/Test/game.exe}\n"
                f"  files:\n  - mod: {{url: '{mod.as_uri()}', filename: pinned-mod.mix, sha256: \"{digest}\"}}\n"
                "  - setup: \"N/A:setup\"\n  installer:\n"
                "  - task: {name: create_prefix, prefix: $GAMEDIR, arch: win32}\n"
                "  - task: {name: wineexec, executable: setup}\n"
                "  - copy: {src: mod, dst: $GAMEDIR/drive_c/Games/Test}\n")
            shutil.rmtree(root / "modlib", ignore_errors=True)
            assert pw_install.main([str(modded), "--library", str(root / "modlib"), "--wine", str(wine),
                                    "--file", f"setup={setup}"]) == expected
            if not expected:
                copied = root / "modlib" / "prefixes" / "modded" / "drive_c/Games/Test/pinned-mod.mix"
                assert copied.read_bytes() == mod.read_bytes()

        # PE architecture from the optional header.
        assert pw_install.pe_architecture(setup) == "pe32"
        (root / "x64.exe").write_bytes(pe(64))
        assert pw_install.pe_architecture(root / "x64.exe") == "pe64"
    print("pw_install passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
