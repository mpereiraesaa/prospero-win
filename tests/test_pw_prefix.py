#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/pw_prefix.py pushes a game to the console and pulls what it changed.

The console is a directory here, behind the operations the FTP remote
provides; the prefix has the links a Wine prefix has. ps5upload's client is
a script that copies into that directory, so nothing touches the network."""
from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import resource
import signal
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

    def write_stream(self, path, stream):
        # As ftplib's storbinary does; runs of zeros stay holes, so a large
        # sparse test file costs no disk on the "console" either.
        with self._path(path).open("wb") as out:
            while chunk := stream.read(pw_prefix.CHUNK):
                assert len(chunk) <= pw_prefix.CHUNK
                if chunk.count(0) == len(chunk):
                    out.seek(len(chunk), os.SEEK_CUR)
                else:
                    out.write(chunk)
            out.truncate()
        self.writes.append(path)

    def read(self, path):
        return self._path(path).read_bytes()

    def read_stream(self, path, take):
        with self._path(path).open("rb") as stream:
            while chunk := stream.read(pw_prefix.CHUNK):
                take(chunk)

    def delete(self, path):
        self._path(path).unlink()


def make_game(root: Path, library: Path, console: Path) -> Path:
    """A Wine prefix with links and registry, and a console with a launcher list."""
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
    return prefix


GAME = "drive_c/Games/Game"
REMOTE_PREFIX = "data/prospero-win/prefixes/game"


def new_game(root: Path, name: str, files: int = 0) -> tuple[Path, Path, list[str]]:
    """A fresh library and console under root/name, with files numbered data files."""
    library, console = root / name / "library", root / name / "console"
    prefix = make_game(root, library, console)
    for index in range(files):
        (prefix / GAME / f"data{index:02}.bin").write_bytes(bytes([index]) * (100 + index))
    cpu_dll = root / name / "wowprospero.dll"
    cpu_dll.write_bytes(b"MZ wowprospero")
    return library, console, ["--library", str(library), "--cpu-dll", str(cpu_dll)]


def manifest(library: Path) -> dict[str, list]:
    return json.loads((library / ".pw/game.json").read_text())["files"]


def game_writes(remote: DirRemote) -> list[str]:
    return [w for w in remote.writes if f"/{GAME}/" in w]


def check_streaming(root: Path) -> None:
    """A file larger than the memory a push may use goes over in chunks."""
    library, console, common = new_game(root, "streaming")
    big = library / "prefixes/game" / GAME / "big.dat"
    with big.open("wb") as out:     # sparse: 1 GiB of mostly holes
        out.truncate(1 << 30)
        out.seek(512 << 20)
        out.write(b"middle")
    whole = pathlib.Path.read_bytes

    def read_bytes(self):
        assert self.stat().st_size < 64 << 20, f"{self} read whole"
        return whole(self)
    pathlib.Path.read_bytes = read_bytes
    try:
        remote = DirRemote(console)
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        stored = console / REMOTE_PREFIX / GAME / "big.dat"
        assert stored.stat().st_size == 1 << 30
        with stored.open("rb") as copy:
            copy.seek(512 << 20)
            assert copy.read(6) == b"middle"
        assert manifest(library)[f"{GAME}/big.dat"][0] == 1 << 30
        # Unchanged: hashed again in chunks, not sent.
        remote.writes.clear()
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        assert not game_writes(remote)
        # A file the console made (a large save, say) is pulled in chunks too.
        made = console / REMOTE_PREFIX / GAME / "save.dat"
        with made.open("wb") as out:
            out.truncate(80 << 20)
            out.write(b"console")
        assert pw_prefix.main(["pull", "game", "--library", str(library)], remote) == 0
        with (big.parent / "save.dat").open("rb") as local:
            assert local.read(7) == b"console" and local.seek(0, os.SEEK_END) == 80 << 20
        assert not (library / ".pw/game.pull").exists()
    finally:
        pathlib.Path.read_bytes = whole
    # Peak memory of the whole test process, 1 GiB file included (KiB).
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    assert peak < 128 << 10, f"peak RSS {peak} KiB"


class StoppingRemote(DirRemote):
    """A console connection that stops the push after a number of files."""

    def __init__(self, root: Path, after: int, stop):
        super().__init__(root)
        self.after, self.stop = after, stop

    def write_stream(self, path, stream):
        if len(self.writes) == self.after:
            self.stop()
        super().write_stream(path, stream)


def check_resume(root: Path) -> None:
    """A push killed or stopped part way resumes, sending only the rest."""
    saved = pw_prefix.SAVE_EVERY_FILES
    pw_prefix.SAVE_EVERY_FILES = 5
    try:
        # Killed outright (SIGKILL, the OOM killer): what was saved survives.
        library, console, common = new_game(root, "killed", files=30)
        pid = os.fork()
        if pid == 0:
            with contextlib.redirect_stdout(io.StringIO()):
                pw_prefix.main(["push", "game", *common], StoppingRemote(console, 12, lambda: os._exit(9)))
            os._exit(0)
        _, status = os.waitpid(pid, 0)
        assert os.waitstatus_to_exitcode(status) == 9
        recorded = manifest(library)
        assert len(recorded) == 10, sorted(recorded)
        remote = DirRemote(console)
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        resent = {w.split(f"/{GAME}/")[1] for w in game_writes(remote)}
        assert len(resent) == 32 - 10 and not {f"{GAME}/{name}" for name in resent} & set(recorded)
        assert len(manifest(library)) == 36

        # Stopped with SIGTERM: everything sent so far is recorded.
        library, console, common = new_game(root, "stopped", files=30)
        stop = StoppingRemote(console, 7, lambda: os.kill(os.getpid(), signal.SIGTERM))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            assert pw_prefix.main(["push", "game", *common], stop) == 130
        assert "push again to resume" in errors.getvalue()
        assert len(manifest(library)) == 7
        assert signal.getsignal(signal.SIGTERM) == signal.SIG_DFL
        remote = DirRemote(console)
        assert pw_prefix.main(["push", "game", *common], remote) == 0
        assert len(game_writes(remote)) == 32 - 7
    finally:
        pw_prefix.SAVE_EVERY_FILES = saved


def check_trust_size(root: Path) -> None:
    """--trust-size takes files the console has at the same size as sent."""
    library, console, common = new_game(root, "trust", files=3)
    prefix = library / "prefixes/game"
    # An earlier push stopped without a manifest: two files made it whole,
    # one did not, and data01.bin has other bytes at the same size.
    on_console = console / REMOTE_PREFIX / GAME
    on_console.mkdir(parents=True)
    (on_console / "data00.bin").write_bytes((prefix / GAME / "data00.bin").read_bytes())
    (on_console / "data01.bin").write_bytes(b"?" * 101)
    (on_console / "data02.bin").write_bytes(b"\x02" * 50)
    (console / REMOTE_PREFIX / "system.reg").write_text("WINE REGISTRY system.reg\n")
    remote = DirRemote(console)
    assert pw_prefix.main(["push", "game", "--trust-size", *common], remote) == 1
    assert pw_prefix.main(["push", "game", "--force", "--trust-size", *common], remote) == 0
    assert sorted(game_writes(remote)) == [f"/{REMOTE_PREFIX}/{GAME}/{name}" for name in
                                            ("data.mpq", "data02.bin", "game.exe")]
    # The registry is always compared and sent, never taken by size.
    assert f"/{REMOTE_PREFIX}/system.reg" in remote.writes
    # Taken on trust: recorded with the PC's hash, the bytes not checked.
    assert (on_console / "data01.bin").read_bytes() == b"?" * 101
    assert manifest(library)[f"{GAME}/data01.bin"] == pw_prefix.file_entry("", prefix / GAME / "data01.bin")
    # Without --trust-size, --force sends everything again.
    library, console, common = new_game(root, "untrusted", files=3)
    (console / REMOTE_PREFIX / GAME).mkdir(parents=True)
    (console / REMOTE_PREFIX / GAME / "data00.bin").write_bytes(b"\x00" * 100)
    remote = DirRemote(console)
    assert pw_prefix.main(["push", "game", "--force", *common], remote) == 0
    assert len(game_writes(remote)) == 5


FAKE_CLIENT = """\
import os, shutil, sys
from pathlib import Path
console = Path(os.environ["FAKE_CONSOLE"])
calls = Path(os.environ["FAKE_CALLS"])
address, command, *rest = sys.argv[1:]
with calls.open("a") as log:
    log.write(" ".join([address, command, *rest]) + "\\n")
if os.environ.get("FAKE_DOWN"):
    sys.exit("Error: connect " + address + ": Connection refused")
# The payload takes transactions on 9113 and everything else on 9114.
if (command in ("transfer", "transfer-dir")) != address.endswith(":9113"):
    print("frame_type=Error")
    print("  body: wrong_port")
    sys.exit("Error: expected HelloAck, got Error")
if command == "hello":
    print("frame_type=HelloAck")
    print('{"version":"2.2.0"}')
elif command == "transfer-dir":
    tx_id, dest, source = rest
    for directory, _, names in os.walk(source):
        for name in names:
            path = Path(directory, name)
            with calls.open("a") as log:
                log.write(f"links={os.stat(path).st_nlink}\\n")
            target = console / dest.lstrip("/") / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    if os.environ.get("FAKE_COMMIT_TIMEOUT"):
        sys.exit("Error: commit ack timed out")
    print('commit_ack: {"tx_id":"%s"}' % tx_id)
    print("frame_type=QueryTxAck")
    print('{"tx_id":"%s","state":"committed","shards_received":1}' % tx_id)
elif command == "query-tx":
    print("frame_type=QueryTxAck")
    print('{"tx_id":"%s","state":"committed"}' % rest[0])
else:
    sys.exit("unknown command " + command)
"""


def check_ps5upload(root: Path) -> None:
    """--transport ps5upload sends the game's files through ps5upload's client."""
    library, console, common = new_game(root, "ps5upload", files=5)
    prefix = library / "prefixes/game"
    client, calls = root / "ps5upload-lab", root / "ps5upload-calls"
    client.write_text(f"#!{sys.executable}\n{FAKE_CLIENT}")
    client.chmod(0o755)
    os.environ.update(FAKE_CONSOLE=str(console), FAKE_CALLS=str(calls))
    fast = ["--transport", "ps5upload", "--ps5upload-client", str(client), "--host", "ps5.invalid", *common]
    saved = pw_prefix.PS5UPLOAD_BATCH_FILES
    pw_prefix.PS5UPLOAD_BATCH_FILES = 4
    try:
        remote = DirRemote(console)
        assert pw_prefix.main(["push", "game", *fast], remote) == 0
        log = calls.read_text().splitlines()
        assert log[0] == "ps5.invalid:9114 hello"
        transfers = [line.split() for line in log if " transfer-dir " in line]
        # Seven game files in batches of four, each its own transaction.
        assert len(transfers) == 2 and transfers[0][2] != transfers[1][2]
        assert all(t[0] == "ps5.invalid:9113" and t[3] == f"/{REMOTE_PREFIX}" for t in transfers)
        # Staged as links to the library's files, not copies, and cleaned up.
        assert [line for line in log if line.startswith("links=")] == ["links=2"] * 7
        assert not (library / ".pw/stage").exists() or not any((library / ".pw/stage").iterdir())
        for name in ("game.exe", "data.mpq", "data03.bin"):
            assert (console / REMOTE_PREFIX / GAME / name).read_bytes() == (prefix / GAME / name).read_bytes()
        # FTP still carries the registry, the links, the CPU DLL and the profile.
        assert not game_writes(remote)
        assert {f"/{REMOTE_PREFIX}/system.reg", f"/{REMOTE_PREFIX}/dosdevices/.pw-symlinks",
                f"/{REMOTE_PREFIX}/drive_c/windows/system32/wowprospero.dll",
                "/data/prospero-win/profiles/game.profile"} <= set(remote.writes)
        assert "wowprospero.dll" in (console / REMOTE_PREFIX / "system.reg").read_text()
        recorded = manifest(library)
        assert recorded[f"{GAME}/data03.bin"] == pw_prefix.file_entry("", prefix / GAME / "data03.bin")
        assert len(recorded) == 11

        # One file changed: only it goes, and nothing over FTP for it.
        (prefix / GAME / "game.exe").write_bytes(b"MZ game v2")
        calls.unlink()
        remote.writes.clear()
        assert pw_prefix.main(["push", "game", *fast], remote) == 0
        assert [line for line in calls.read_text().splitlines() if line.startswith("links=")] == ["links=2"]
        assert (console / REMOTE_PREFIX / GAME / "game.exe").read_bytes() == b"MZ game v2"
        assert not game_writes(remote)

        # The client gave up waiting for the commit: the management port
        # says it went through.
        (prefix / GAME / "game.exe").write_bytes(b"MZ game v3")
        os.environ["FAKE_COMMIT_TIMEOUT"] = "1"
        calls.unlink()
        assert pw_prefix.main(["push", "game", *fast], remote) == 0
        assert any(line.startswith("ps5.invalid:9114 query-tx ") for line in calls.read_text().splitlines())
        del os.environ["FAKE_COMMIT_TIMEOUT"]

        # Without the payload running, the push stops before sending anything.
        os.environ["FAKE_DOWN"] = "1"
        (prefix / GAME / "game.exe").write_bytes(b"MZ game v4")
        remote.writes.clear()
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            assert pw_prefix.main(["push", "game", *fast], remote) == 1
        assert "ps5upload's payload does not answer on ps5.invalid:9114" in errors.getvalue()
        assert "Connection refused" in errors.getvalue() and not remote.writes
        del os.environ["FAKE_DOWN"]
        # Nor without the client.
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            assert pw_prefix.main(["push", "game", *fast, "--ps5upload-client", str(root / "absent")], remote) == 1
        assert "--ps5upload-client" in errors.getvalue()
    finally:
        pw_prefix.PS5UPLOAD_BATCH_FILES = saved


def check_console_user(root: Path) -> None:
    """A prefix made as another user gets the console user's folders on the console."""
    library, console, fast = new_game(root, "other-user")
    prefix = library / "prefixes/game"
    os.rename(prefix / "drive_c/users/prospero", prefix / "drive_c/users/someone")
    remote = DirRemote(console)
    with contextlib.redirect_stdout(io.StringIO()) as output:
        assert pw_prefix.main(["push", "game", *fast], remote) == 0
    assert "no drive_c/users/prospero" in output.getvalue()
    users = console / REMOTE_PREFIX / "drive_c/users"
    for directory in pw_prefix.CONSOLE_USER_DIRS:
        assert (users / "prospero" / directory).is_dir()
    assert (users / "someone/Desktop").is_dir()
    # A prefix that has the console user's folders is pushed as it is.
    library, console, fast = new_game(root, "same-user")
    remote = DirRemote(console)
    with contextlib.redirect_stdout(io.StringIO()) as output:
        assert pw_prefix.main(["push", "game", *fast], remote) == 0
    assert "no drive_c/users/prospero" not in output.getvalue()
    assert not (console / REMOTE_PREFIX / "drive_c/users/prospero/AppData").exists()


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        library, console = root / "library", root / "console"
        prefix = make_game(root, library, console)
        remote = DirRemote(console)
        common = ["--library", str(library)]
        remote_prefix = console / "data/prospero-win/prefixes/game"

        # Without wowprospero.dll a 32-bit game could not start: the first
        # push asks for it, then puts it in the prefix's system32.
        assert pw_prefix.main(["push", "game", *common], remote) == 1
        cpu_dll = root / "wowprospero.dll"
        cpu_dll.write_bytes(b"MZ wowprospero")
        assert pw_prefix.main(["push", "game", "--cpu-dll", str(cpu_dll), *common], remote) == 0
        assert (remote_prefix / "drive_c/windows/system32/wowprospero.dll").read_bytes() == b"MZ wowprospero"
        assert not (prefix / "drive_c/windows/system32/wowprospero.dll").exists()
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
        assert (remote_prefix / "drive_c/windows/system32/wowprospero.dll").exists()

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

        check_streaming(root)
        check_resume(root)
        check_trust_size(root)
        check_ps5upload(root)
        check_console_user(root)
    print("pw_prefix passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
