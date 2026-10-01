#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""pw_gameplay_run.py against a console library kept in a directory."""
from __future__ import annotations

import contextlib
import ftplib
import io
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True  # no tools/__pycache__ for the publication audit
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pw_gameplay_run as run  # noqa: E402

GAME = """PW_REPORT/1 build=abc profile=cs16 cycle=0 pid=7 time=1790000000
REC seq=1 t=1.0 PW_WINE64 profile id=cs16
REC seq=2 t=2.0 WINESERVER PW_GL frames=13 fps=2.6
REC seq=3 t=3.0 WINESERVER PW_GL frames=100 fps=20.0
REC seq=4 t=4.0 WINESERVER PW_GL frames=280 fps=56.0
REC seq=5 t=5.0 PW_WINE64 script key=0xd status=0/0
REC seq=6 t=6.0 WINESERVER PW_GL frames=300 fps=60.0
REC seq=7 t=7.0 WINESERVER PW_GL frames=250 fps=50.0
REC seq=8 t=8.0 WINESERVER wowprospero timing: tid=0024 run=40.0% unix=50.0% (1/s 1.0us) sys=10.0%
REC seq=9 t=9.0 WINESERVER wowprospero timing: tid=0030 run=0.8% unix=0.0% (0/s 0.0us) sys=99.2%
REC seq=10 t=10.0 PW_WINE64 session_end reason=close-timeout
"""


class Console:
    """The library root as the tool sees it over FTP; the title's run happens
    when the tool first polls for it."""

    def __init__(self, root: Path, finish: bool = True):
        self.root, self.finish, self.polls, self.during = root, finish, 0, {}

    def _path(self, path: str) -> Path:
        return self.root / path.lstrip("/").removeprefix("data/prospero-win/")

    def read(self, path):
        target = self._path(path)
        if path.endswith("logs/next.txt"):
            self.polls += 1
            if self.polls == 2:  # the run: snapshot what the title would see
                for name in ("profiles/profiles.lst", "profiles/counter-strike-16.profile", "pw_script_keys", "pw_wow_timing",
                             "prefix/drive_c/Games/cs/listenserver.cfg"):
                    entry = self.root / name
                    self.during[name] = entry.read_bytes() if entry.exists() else None
                if self.finish:
                    (self.root / "logs/session-3.log").write_text("PW_REPORT/1 build=abc profile=launcher\n"
                                                                  "REC seq=1 t=1 PW_WINE64 session_end reason=launcher\n")
                    (self.root / "logs/session-4.log").write_text(GAME)
                    target.write_text("5\n")
        if not target.is_file():
            raise ftplib.error_perm("550 not found")
        return target.read_bytes()

    def write(self, path, data):
        self._path(path).write_bytes(data)

    def delete(self, path):
        target = self._path(path)
        if not target.exists():
            raise ftplib.error_perm("550 not found")
        target.unlink()
        raise ftplib.error_reply("226 File deleted")  # what ftpsrv answers


def library(root: Path) -> None:
    (root / "profiles").mkdir(parents=True)
    (root / "profiles/counter-strike-16.profile").write_text("[application]\nid = cs16\nname = CS\n")
    (root / "profiles/profiles.lst").write_text("half-life.profile\ncounter-strike-16.profile\n")
    (root / "logs").mkdir()
    (root / "logs/next.txt").write_text("3\n")
    (root / "prefix/drive_c/Games/cs").mkdir(parents=True)
    (root / "prefix/drive_c/Games/cs/listenserver.cfg").write_bytes(b"sv_cheats 0\r\nexec x.cfg\r\n")


def main(console, *args) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = run.main(["counter-strike-16", "--poll", "0", *args], remote=console)
    return code, out.getvalue()


# Keys: names and virtual-key codes, sorted by time; malformed ones refused.
assert run.parse_key("25000:enter") == (25000, 0x0D)
assert run.parse_key("700:2") == (700, 0x32) and run.parse_key("1:F5") == (1, 0x74)
assert run.parse_key("10:a") == (10, 0x41) and run.parse_key("10:0x1b") == (10, 0x1B)
for bad in ("enter", "x:enter", "10:", "10:nokey", "10:0x123"):
    try:
        run.parse_key(bad)
        raise AssertionError(bad)
    except ValueError:
        pass
assert run.key_script([(700, 0x32), (0, 0x0D)]) == b"0 0x0d\n700 0x32\n"
assert run.appended(b"a\r\nb\r\n", ["c"]) == b"a\r\nb\r\nc\r\n"
assert run.appended(b"a", ["b", "c"]) == b"a\nb\nc\n" and run.appended(b"", ["x"]) == b"x\n"
profile = b"[application]\nid = cs16\narguments = -game cstrike\nname = CS\n"
assert run.with_arguments(profile, "-game cstrike +map de_dust2") == \
    b"[application]\nid = cs16\narguments = -game cstrike +map de_dust2\nname = CS\n"
assert run.with_arguments(b"[application]\r\nid = x\r\n", "+map a") == b"[application]\r\narguments = +map a\nid = x\r\n"
for bad in ("noequals", "/abs=x", "a/../b=x"):
    try:
        run.parse_append(bad)
        raise AssertionError(bad)
    except ValueError:
        pass

# A run: the console sees one game, the keys, the timing trigger and the
# appended config; afterwards everything is as it was, and the report reads
# the game's session (by profile id), not the launcher's.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    console = Console(root)
    saved = Path(directory) / "saved"
    code, output = main(console, "--key", "25000:enter", "--key", "25700:2", "--timing",
                        "--arguments", "-game cstrike +map de_dust2",
                        "--append", "prefix/drive_c/Games/cs/listenserver.cfg=bot_quota 9",
                        "--save", str(saved))
    assert code == 0, output
    during = console.during
    assert during["profiles/profiles.lst"] == b"counter-strike-16.profile\n"
    assert during["profiles/counter-strike-16.profile"] == b"[application]\narguments = -game cstrike +map de_dust2\nid = cs16\nname = CS\n"
    assert (root / "profiles/counter-strike-16.profile").read_text() == "[application]\nid = cs16\nname = CS\n"
    assert during["pw_script_keys"] == b"25000 0x0d\n25700 0x32\n"
    assert during["pw_wow_timing"] == b"timing\n"
    assert during["prefix/drive_c/Games/cs/listenserver.cfg"] == b"sv_cheats 0\r\nexec x.cfg\r\nbot_quota 9\r\n"
    assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
    assert not (root / "pw_script_keys").exists() and not (root / "pw_wow_timing").exists()
    assert (root / "prefix/drive_c/Games/cs/listenserver.cfg").read_bytes() == b"sv_cheats 0\r\nexec x.cfg\r\n"
    assert (saved / "counter-strike-16-1790000000.log").read_text() == GAME
    assert "keys sent: 1 (0xd:0/0)" in output
    assert "fps after loading: average 55.0, minimum 50.0, at 59 or more 1/2" in output
    assert "timing: tid=0024 run=40.0%" in output and "tid=0030" not in output
    assert "ended: close-timeout" in output

# No run: the tool gives up after --wait and still restores the console.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    code, output = main(Console(root, finish=False), "--key", "1:enter", "--wait", "0")
    assert code == 1 and "no finished counter-strike-16 session" in output
    assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
    assert not (root / "pw_script_keys").exists()

# A missing game or config file stops before anything is changed for good.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    for args in (["--append", "prefix/missing.cfg=x"],):
        try:
            main(Console(root), *args)
            raise AssertionError(args)
        except SystemExit:
            pass
        assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
        assert not (root / "pw_script_keys").exists()

print("pw_gameplay_run passed: keys, launch arguments, appended configs, timing trigger, restore on success/timeout/refusal, session by profile id, summary")
