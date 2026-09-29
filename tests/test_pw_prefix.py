#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/pw_prefix.py pushes a game to the console and pulls what it changed.

The console is a directory here, behind the same five operations the FTP
remote provides; the prefix has the links a Wine prefix has."""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pw_prefix  # noqa: E402


class DirRemote:
    """A console's filesystem in a directory, as FtpRemote presents it."""

    def __init__(self, root: Path):
        self.root = root
        self.writes: list[str] = []

    def _path(self, path: str) -> Path:
        return self.root / path.lstrip("/")

    def listdir(self, path):
        return {entry.name: ("dir" if entry.is_dir() else "file", entry.stat().st_size if entry.is_file() else 0)
                for entry in os.scandir(self._path(path))}

    def exists(self, path):
        return self._path(path).exists()

    def size(self, path):
        target = self._path(path)
        return target.stat().st_size if target.is_file() else None

    def makedirs(self, path):
        self._path(path).mkdir(parents=True, exist_ok=True)

    def write(self, path, data):
        self._path(path).write_bytes(data)
        self.writes.append(path)

    def read(self, path):
        return self._path(path).read_bytes()

    def delete(self, path):
        self._path(path).unlink()


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        library, console = root / "library", root / "console"
        prefix = library / "prefixes" / "game"
        (prefix / "drive_c/Games/Game").mkdir(parents=True)
        (prefix / "drive_c/users/prospero").mkdir(parents=True)
        (prefix / "dosdevices").mkdir()
        (prefix / "drive_c/Games/Game/game.exe").write_bytes(b"MZ game")
        (prefix / "drive_c/Games/Game/data.mpq").write_bytes(b"x" * 1000)
        for name in pw_prefix.REGISTRY:
            (prefix / name).write_text(f"WINE REGISTRY {name}\n")
        cpu = '[Software\\\\Microsoft\\\\Wow64\\\\x86] 1\n@="wow64cpu.dll"\n\n[Other]\n@="wow64cpu.dll"\n'
        (prefix / "system.reg").write_text("WINE REGISTRY system.reg\n" + cpu)
        os.symlink("../drive_c", prefix / "dosdevices/c:")
        os.symlink("/", prefix / "dosdevices/z:")
        os.symlink("/dev/ttyS0", prefix / "dosdevices/com1")
        os.symlink(str(root), prefix / "drive_c/users/prospero/Desktop")
        (prefix / ".wineserver").mkdir()
        (prefix / ".wineserver/lock").write_text("")
        (library / "profiles").mkdir(parents=True)
        (library / "profiles/game.profile").write_text("[application]\nid = game\n")
        (console / "data/prospero-win/profiles").mkdir(parents=True)
        (console / "data/prospero-win/profiles/profiles.lst").write_text("pinball.profile\n")
        remote = DirRemote(console)
        common = ["--library", str(library)]
        remote_prefix = console / "data/prospero-win/prefixes/game"

        assert pw_prefix.main(["push", "game", *common], remote) == 0
        assert (remote_prefix / "drive_c/Games/Game/game.exe").read_bytes() == b"MZ game"
        # The console's CPU backend is written on the way, the PC's is kept.
        console_reg = (remote_prefix / "system.reg").read_text()
        assert console_reg.count("wowprospero.dll") == 1 and '[Other]\n@="wow64cpu.dll"' in console_reg
        assert (prefix / "system.reg").read_text().count("wowprospero") == 0
        assert (remote_prefix / "dosdevices/.pw-symlinks").read_text() == "c:\t../drive_c\nz:\t/\n"
        assert (remote_prefix / "drive_c/users/prospero/Desktop").is_dir()
        assert not (remote_prefix / ".wineserver").exists()
        assert not (remote_prefix / "dosdevices/com1").exists()
        assert (console / "data/prospero-win/profiles/game.profile").is_file()
        assert (console / "data/prospero-win/profiles/profiles.lst").read_text() == "pinball.profile\ngame.profile\n"

        # Nothing changed: a second push sends nothing and lists the game once.
        remote.writes.clear()
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        assert not [w for w in remote.writes if "/prefixes/game/drive_c" in w or w.endswith(".reg")]
        assert (console / "data/prospero-win/profiles/profiles.lst").read_text().count("game.profile") == 1

        # One file changed on the PC: only it is sent.
        (prefix / "drive_c/Games/Game/game.exe").write_bytes(b"MZ game v2")
        remote.writes.clear()
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        assert [w for w in remote.writes if "/drive_c/" in w] == ["/data/prospero-win/prefixes/game/drive_c/Games/Game/game.exe"]

        # The game saved settings on the console: a push refuses until a pull.
        (remote_prefix / "user.reg").write_text("WINE REGISTRY user.reg\n[Software\\\\Game]\n\"volume\"=dword:5\n")
        (remote_prefix / "drive_c/Games/Game/save1.sav").write_bytes(b"save")
        assert pw_prefix.main(["push", "game", *common], remote) == 1
        assert pw_prefix.main(["pull", "game", *common], remote) == 0
        assert "volume" in (prefix / "user.reg").read_text()
        # A pull brings system.reg back with the PC's backend, and does not
        # count the swap as a change.
        assert "wowprospero" not in (prefix / "system.reg").read_text()
        assert (prefix / "drive_c/Games/Game/save1.sav").read_bytes() == b"save"
        assert pw_prefix.main(["push", "game", *common], remote) == 0

        # --delete removes what the PC no longer has; without it, it stays.
        (prefix / "drive_c/Games/Game/data.mpq").unlink()
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        assert (remote_prefix / "drive_c/Games/Game/data.mpq").exists()
        (prefix / "drive_c/Games/Game/data.mpq").write_bytes(b"x" * 1000)
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        (prefix / "drive_c/Games/Game/data.mpq").unlink()
        assert pw_prefix.main(["push", "game", "--delete", *common], remote) == 0
        assert not (remote_prefix / "drive_c/Games/Game/data.mpq").exists()

        # A console prefix this PC never synced is not overwritten.
        other = root / "other-library"
        (other / "prefixes/game").mkdir(parents=True)
        (other / "profiles").mkdir()
        (other / "profiles/game.profile").write_text("")
        assert pw_prefix.main(["push", "game", "--library", str(other)], remote) == 1
        # Nor is a game that was not installed pushed.
        assert pw_prefix.main(["push", "absent", *common], remote) == 1
        # Without a console address there is nothing to do.
        assert pw_prefix.main(["status", "game", *common]) == 2 or os.environ.get("PS5_HOST")
    print("pw_prefix passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
