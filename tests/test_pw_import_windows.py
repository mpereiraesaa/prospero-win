#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A Windows import produces the same prefix/profile layout that FTP push reads."""
from __future__ import annotations

import hashlib
import io
import json
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pw_import_windows as importer  # noqa: E402
from pw_prefix import local_tree  # noqa: E402


def small_pe() -> bytes:
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x80 + 24, 0x10b)
    return bytes(data)


class WindowsImportTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.template = self.root / "template.zip"
        self.game = self.root / "game"
        self.game.mkdir()
        (self.game / "Game.exe").write_bytes(small_pe())
        (self.game / "Data").mkdir()
        (self.game / "Data" / "game.dat").write_bytes(b"installed game data")
        cpu = b"console CPU DLL"
        metadata = {"format": importer.TEMPLATE_FORMAT, "wine": importer.EXPECTED_WINE,
                    "user": "prospero", "cpu_dll_sha256": hashlib.sha256(cpu).hexdigest()}
        with zipfile.ZipFile(self.template, "w") as archive:
            archive.writestr("template.json", json.dumps(metadata))
            archive.writestr("prefix/system.reg", "WINE REGISTRY Version 2\n\n[Software\\\\Wine] 1\n\n")
            archive.writestr("prefix/user.reg", "WINE REGISTRY Version 2\n\n[Software\\\\Wine\\\\DllOverrides] 1\n\n")
            archive.writestr("prefix/userdef.reg", "WINE REGISTRY Version 2\n")
            archive.writestr("prefix/drive_c/windows/system32/wowprospero.dll", cpu)
            archive.writestr("prefix/dosdevices/.pw-symlinks", "c:\t../drive_c\nz:\t/\n")

    def import_args(self, *extra: str) -> list[str]:
        return ["import", "--template", str(self.template), "--game-dir", str(self.game),
                "--exe", "Game.exe", "--install-path", r"C:\Games\The Game",
                "--library", str(self.root / "library"), "--slug", "the-game",
                "--name", "The Game", *extra]

    def test_import_uses_template_links_and_game_files(self) -> None:
        preset = self.root / "game.input"
        preset.write_text("[input]\nmode = keyboard\n", encoding="utf-8")
        self.assertEqual(importer.main(self.import_args(
            "--graphics", "gdi", "--input-mode", "keyboard", "--input-preset", str(preset),
            "--reg-string", r"HKLM\Software\The Game|Installed Path|C:\Games\The Game")), 0)
        prefix = self.root / "library/prefixes/the-game"
        self.assertEqual((prefix / "drive_c/Games/The Game/Data/game.dat").read_bytes(), b"installed game data")
        files, _, links = local_tree(prefix)
        self.assertIn("drive_c/Games/The Game/Game.exe", files)
        self.assertEqual(links["dosdevices"], {"c:": "../drive_c", "z:": "/"})
        profile = (self.root / "library/profiles/the-game.profile").read_text()
        self.assertIn(r"executable = C:\Games\The Game\Game.exe", profile)
        self.assertIn("architecture = pe32", profile)
        self.assertIn("preset = game", profile)
        self.assertTrue((self.root / "library/input/game.input").is_file())
        self.assertIn('"Installed Path"="C:\\\\Games\\\\The Game"', (prefix / "system.reg").read_text())

    def test_import_installs_dxvk_and_rejects_duplicate(self) -> None:
        archive = self.root / "dxvk.tar.gz"
        with tarfile.open(archive, "w:gz") as release:
            for bits in ("x32", "x64"):
                for dll in importer.DXVK_DLLS:
                    payload = f"{bits}/{dll}".encode()
                    member = tarfile.TarInfo(f"dxvk-test/{bits}/{dll}.dll")
                    member.size = len(payload)
                    release.addfile(member, io.BytesIO(payload))
        importer.DXVK["test"] = importer.digest(archive)
        self.addCleanup(importer.DXVK.pop, "test")
        args = self.import_args("--graphics", "dxvk", "--dxvk-version", "test",
                                "--dxvk-archive", str(archive))
        self.assertEqual(importer.main(args), 0)
        prefix = self.root / "library/prefixes/the-game"
        self.assertEqual((prefix / "drive_c/windows/syswow64/d3d9.dll").read_bytes(), b"x32/d3d9")
        self.assertEqual((prefix / "drive_c/windows/system32/d3d9.dll").read_bytes(), b"x64/d3d9")
        self.assertIn('"d3d9"="native"', (prefix / "user.reg").read_text())
        self.assertIn("dll_overrides = d3d8,d3d9,d3d10core,d3d11,dxgi=n",
                      (self.root / "library/profiles/the-game.profile").read_text())
        self.assertEqual(importer.main(args), 1)

    def test_rejects_nonportable_paths_and_bad_template(self) -> None:
        with self.assertRaises(importer.ImportError_):
            importer.target_parts(r"C:\Games\..\Escape")
        with self.assertRaises(importer.ImportError_):
            importer.relative_parts(r"..\Game.exe")
        with zipfile.ZipFile(self.template, "a") as archive:
            archive.writestr("prefix/../escape.txt", b"bad")
        self.assertEqual(importer.main(self.import_args("--graphics", "gdi")), 1)
        self.assertFalse((self.root / "library/prefixes/the-game").exists())
        self.assertFalse((self.root / "escape.txt").exists())


if __name__ == "__main__":
    unittest.main()
