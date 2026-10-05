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
APP = "data/homebrew/APP"  # the app's folder, as the fake console stores it
SIGNED = bytes.fromhex("5414f5ee") + b"candidate translator"  # a signed module
APP_BINARY = bytes.fromhex("4f153d1d") + b"an eboot, not a module"
EXECUTION = ("REC seq=9 t=9.5 WINESERVER wowprospero execution: tid=0024 cumulative=1 sample_cpu_ns=1508594000 "
             "calls=32265545 samples=504504 stride=64 clock_batch_read_ns=852 clock_resolution_ns=1000 clock_errors=0\n")


class Console:
    """The library root as the tool sees it over FTP; the title's run happens
    when the tool first polls for it."""

    def __init__(self, root: Path, finish: bool = True, stop: bool = False, broken: bool = False):
        self.root, self.finish, self.stop, self.polls, self.during = root, finish, stop, 0, {}
        self.broken, self.reconnects = broken, 0

    def reconnect(self):
        self.broken = False
        self.reconnects += 1

    def _path(self, path: str) -> Path:
        return self.root / path.lstrip("/").removeprefix("data/prospero-win/")

    def read(self, path):
        target = self._path(path)
        if path.endswith("logs/next.txt"):
            self.polls += 1
            if self.polls == 2:  # the run: snapshot what the title would see
                for name in ("profiles/profiles.lst", "profiles/counter-strike-16.profile", "pw_script_keys", "pw_script_input", "pw_wow_timing",
                             "pw_wow_exec_timing", "pw_wow_profile", "pw_wow_dispatch_profile",
                             "prefix/drive_c/Games/cs/listenserver.cfg", APP + "/" + run.RUNTIME):
                    entry = self.root / name
                    self.during[name] = entry.read_bytes() if entry.exists() else None
                if self.stop:  # SIGTERM while the game runs (exit_on_signal)
                    raise SystemExit(143)
                if self.finish:
                    (self.root / "prefix/drive_c/Games/cs/demo.log").write_text("5182 frames 86.6 seconds 59.8 fps\n")
                    (self.root / "logs/session-3.log").write_text("PW_REPORT/1 build=abc profile=launcher\n"
                                                                  "REC seq=1 t=1 PW_WINE64 session_end reason=launcher\n")
                    (self.root / "logs/session-4.log").write_text(GAME)
                    target.write_text("5\n")
        if not target.is_file():
            raise ftplib.error_perm("550 not found")
        return target.read_bytes()

    def write(self, path, data):
        if self.broken and self.polls >= 2:  # the signal cut a transfer short
            raise ftplib.error_reply("200 Type set to I")
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
    (root / APP / run.RUNTIME).parent.mkdir(parents=True)
    (root / APP / run.RUNTIME).write_bytes(b"the app's own translator")


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
# --fps: Wine's fps channel joins the profile's own channels, or the title's.
assert run.with_fps_channel(b"[application]\nid = x\n") == \
    b"[application]\nid = x\n[debug]\nwinedebug = err+all,+loaddll,+process,+fps\n"
assert run.with_fps_channel(b"[debug]\r\nwinedebug = +seh ; one run\r\n") == b"[debug]\r\nwinedebug = +seh,+fps ; one run\r\n"
assert run.with_fps_channel(b"[debug]\nwinedebug = -all,+fps\n") == b"[debug]\nwinedebug = -all,+fps\n"

# A Vulkan game's samples: those before the first at 20 fps or more are loading.
VULKAN = "\n".join(f"REC seq={i} t={i}.0 WINE 0024:trace:fps:win32u_vkQueuePresentKHR 0x7f00 @ approx {v}fps, total 1.00fps"
                   for i, v in enumerate(["0.10", "0.70", "59.90", "48.00", "60.01"]))
summary = run.summarize(VULKAN)
assert "fps (Vulkan, 5 samples): 0.1 0.7 59.9 48.0 60.0" in summary
assert "fps after loading: average 56.0, minimum 48.0, at 59 or more 2/3" in summary
assert "fps after loading: none (never 20 or more)" in run.summarize(VULKAN.split("\n", 1)[0])
assert "fps: no PW_GL or Wine fps records (it never presented, or ran without --fps)" in run.summarize("")

# Successive FTP snapshots keep the beginning after more than two chunks
# rotate away, without double-counting overlap or admitting a partial record.
header = GAME.split("\n", 1)[0]
capture = run.SessionCapture(header)
rows = GAME.splitlines(keepends=True)[1:]
capture.add((header + "\n" + "".join(rows[:4]) + rows[4][:20]).encode())
assert len(capture.records) == 4 and not capture.finished
capture.add((header + "\n" + "".join(rows[2:8])).encode())
capture.add((header + "\n" + "".join(rows[6:])).encode())
assert capture.text() == GAME and capture.finished
capture.add((header.replace("pid=7", "pid=8") + "\n" + rows[0]).encode())
assert capture.text() == GAME  # a stale previous chunk belongs to another run
raw_fault = "PW_WINE64 fault signal=0x000000000000000b\n"
capture.add((header + "\n" + raw_fault).encode())
capture.add((header + "\n" + raw_fault).encode())
assert capture.text().count(raw_fault) == 1
try:
    capture.add((header + "\n" + rows[0].replace("profile id=cs16", "profile id=other")).encode())
    raise AssertionError("conflicting record accepted")
except SystemExit as stop:
    assert "conflicting records" in str(stop)
bounded = run.SessionCapture(header)
bounded.bytes = 64 * 1024 * 1024  # exercise admission without allocating 64 MiB
try:
    bounded.add((header + "\n" + rows[0]).encode())
    raise AssertionError("capture budget ignored")
except SystemExit as stop:
    assert "exceeded 64 MiB" in str(stop)
assert "0 missing between records" in "\n".join(run.summarize(GAME))
assert "1 missing between records" in "\n".join(run.summarize(GAME.replace(rows[4], "")))

class RotatingConsole(Console):
    """Advance an active game on each poll; only two chunks remain on disk."""

    def read(self, path):
        if path.endswith("logs/next.txt"):
            self.polls += 1
            if self.polls >= 2:
                # Start at a process-wide sequence other than one, as real
                # launcher/game sessions do. Finish on the fourth snapshot.
                stage = min(self.polls - 2, 3)
                chunks = [[row.replace(f"seq={n} ", f"seq={n + 90} ")
                           for n, row in enumerate(rows, 1)][a:b]
                          for a, b in [(0, 3), (3, 6), (6, 8), (8, 10)]]
                (self.root / "logs/session-3.log").write_text(header + "\n" + "".join(chunks[stage]))
                if stage:
                    (self.root / "logs/session-3.previous.log").write_text(header + "\n" + "".join(chunks[stage - 1]))
                (self.root / "logs/next.txt").write_text("4\n")
            return (self.root / "logs/next.txt").read_bytes()
        return super().read(path)

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    saved = Path(directory) / "saved"
    console = RotatingConsole(root)
    code, output = main(console, "--save", str(saved))
    assert code == 0, output
    collected = (saved / "counter-strike-16-1790000000.log").read_text()
    assert len(run.re.findall(r"(?m)^REC seq=", collected)) == 10
    assert "seq=91 " in collected and "seq=100 " in collected
    assert "fps after loading: average 55.0" in output and "0 missing between records" in output
    assert "seq=91 " not in (root / "logs/session-3.previous.log").read_text()
    assert not (root / "pw_script_keys").exists()

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

# --fps with --arguments: both reach the profile for the run, which is put back.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    console = Console(root)
    code, output = main(console, "--fps", "--arguments", "+map c1a0")
    assert code == 0, output
    assert console.during["profiles/counter-strike-16.profile"] == \
        b"[application]\narguments = +map c1a0\nid = cs16\nname = CS\n[debug]\nwinedebug = err+all,+loaddll,+process,+fps\n"
    assert (root / "profiles/counter-strike-16.profile").read_text() == "[application]\nid = cs16\nname = CS\n"

# --input: the macro is on the console for the run only, and the summary
# says how much of it was replayed.
assert run.summarize("REC seq=1 t=1 PW_WINE64 script input status=ok events=3 bad_line=0 sync=a.log\n"
                     "REC seq=2 t=2 PW_WINE64 script input synced at offset=10\n"
                     "REC seq=3 t=3 PW_WINE64 script input replayed=3 of 3\n")[0] == \
    "macro: ok, 3 of 3 events replayed (synced)"
assert run.summarize("REC seq=1 t=1 PW_WINE64 script input status=malformed events=0 bad_line=7 sync=-\n")[0] == \
    "macro: malformed, 0 of 0 events replayed, refused at line 7 (never synced)"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    macro = Path(directory) / "macro.txt"
    macro.write_bytes(b"sync logs/game.log loaded\n0 key 0x57 1\n500 move 5 0\n900 key 0x57 0\n")
    console = Console(root)
    code, output = main(console, "--input", str(macro))
    assert code == 0, output
    assert console.during["pw_script_input"] == macro.read_bytes()
    assert not (root / "pw_script_input").exists()
    assert "a recorded macro" in output

# --pad: names (any case), masks and hold times; malformed ones refused.
assert run.parse_pad("45000:a") == (45000, "a", 150)
assert run.parse_pad("0:START:300") == (0, "start", 300) and run.parse_pad("5:lb:0") == (5, "lb", 0)
assert run.parse_pad("5:0x1010") == (5, "0x1010", 150) and run.parse_pad("5:0X1") == (5, "0x0001", 150)
assert {run.parse_pad(f"1:{name}")[1] for name in run.PAD_BUTTONS} == set(run.PAD_BUTTONS)
for bad in ("a", "x:a", "10:", "10:select", "10:a:", "10:a:-5", "10:a:1:2", "10:0x0800", "10:0x0",
            "10:0x10000", "10:4096", "-1:a"):
    try:
        run.parse_pad(bad)
        raise AssertionError(bad)
    except ValueError:
        pass
# The macro: a press and a release per --pad, merged with --input's events
# by time, the sync line first; without --pad the macro goes as it is.
assert run.macro_script(None, [(45000, "a", 150)]) == b"45000 pad a 1\n45150 pad a 0\n"
assert run.macro_script(b"0 key 0x57 1\n", []) == b"0 key 0x57 1\n"
assert run.macro_script(None, [(900, "b", 150), (100, "a", 1000)]) == \
    b"100 pad a 1\n900 pad b 1\n1050 pad b 0\n1100 pad a 0\n"
assert run.macro_script(b"sync logs/game.log loaded\r\n# W held\r\n0 key 0x57 1\r\n\r\n"
                        b"500  move 5 0\r\n900 key 0x57 0\r\n", [(500, "start", 0), (100, "0x1010", 400)]) == \
    (b"sync logs/game.log loaded\n0 key 0x57 1\n100 pad 0x1010 1\n500 move 5 0\n"
     b"500 pad 0x1010 0\n500 pad start 1\n500 pad start 0\n900 key 0x57 0\n")
for bad_macro in (b"0 key 0x57 1\nsync a.log t\n", b"jump 1\n"):
    try:
        run.macro_script(bad_macro, [(1, "a", 150)])
        raise AssertionError(bad_macro)
    except SystemExit:
        pass
# The summary: the controller buttons the title held, and a profile that
# can't take them.
summary = run.summarize("REC seq=1 t=1 PW_WINE64 script input status=ok events=2 bad_line=0 sync=- pad_events=2\n"
                        "REC seq=2 t=2 PW_WINE64 script pad buttons=0x1000 at_ms=45000\n"
                        "REC seq=3 t=3 PW_WINE64 script pad buttons=0 at_ms=45150\n"
                        "REC seq=4 t=4 PW_WINE64 script input replayed=2 of 2\n")
assert summary[0] == "macro: ok, 2 of 2 events replayed (never synced)"
assert summary[1] == "pad buttons held: 2 changes (0x1000 at 45000 ms, 0 at 45150 ms)"
assert "pad: ignored, the game's profile does not use [input] mode = xinput" in \
    run.summarize("REC seq=1 t=1 PW_WINE64 script pad events ignored: the profile's [input] mode is not xinput\n")
assert not any(line.startswith("pad") for line in run.summarize(GAME))
# A run with --pad alone, and with --input: the merged macro is on the
# console for the run only.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    console = Console(root)
    code, output = main(console, "--pad", "45000:a", "--pad", "50000:start:300")
    assert code == 0, output
    assert console.during["pw_script_input"] == b"45000 pad a 1\n45150 pad a 0\n50000 pad start 1\n50300 pad start 0\n"
    assert console.during["pw_script_keys"] == b""
    assert not (root / "pw_script_input").exists()
    assert "2 pad presses" in output and "a recorded macro" not in output
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    macro = Path(directory) / "macro.txt"
    macro.write_bytes(b"0 key 0x57 1\n900 key 0x57 0\n")
    console = Console(root)
    code, output = main(console, "--input", str(macro), "--pad", "500:x:100", "--key", "1000:enter")
    assert code == 0, output
    assert console.during["pw_script_input"] == b"0 key 0x57 1\n500 pad x 1\n600 pad x 0\n900 key 0x57 0\n"
    assert console.during["pw_script_keys"] == b"1000 0x0d\n"
    assert not (root / "pw_script_input").exists()
    assert "a recorded macro, 1 pad presses" in output

# No run: the tool gives up after --wait and still restores the console.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    code, output = main(Console(root, finish=False), "--key", "1:enter", "--wait", "0")
    assert code == 1 and "no finished counter-strike-16 session" in output
    assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
    assert not (root / "pw_script_keys").exists()

# Stale triggers from an interrupted run are removed, not kept for the next
# run (a leftover pw_wow_timing slowed every game down).
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    (root / "pw_wow_timing").write_text("timing\n")
    (root / "pw_script_keys").write_text("0 0x0d\n")
    console = Console(root)
    code, output = main(console, "--key", "1:esc")
    assert code == 0, output
    assert "removing a stale pw_wow_timing (an interrupted run left it)" in output
    assert "removing a stale pw_script_keys" in output
    assert console.during["pw_wow_timing"] is None and console.during["pw_script_keys"] == b"1 0x1b\n"
    assert not (root / "pw_wow_timing").exists() and not (root / "pw_script_keys").exists()

# Profilers and another translator: all on the console for the run only;
# the summary estimates translated CPU, and --fetch copies the game's own
# result next to the saved session.
assert run.summarize(EXECUTION)[-2] == "translated CPU: tid=0024 96.5 s"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    candidate = Path(directory) / "wowprospero.prx"
    candidate.write_bytes(SIGNED)
    saved = Path(directory) / "saved"
    console = Console(root)
    code, output = main(console, "--profiler", "exec-timing", "--profiler", "dispatch-profile", "--timing",
                        "--runtime", str(candidate), "--app", "/" + APP + "/",
                        "--fetch", "prefix/drive_c/Games/cs/demo.log", "--save", str(saved))
    assert code == 0, output
    during = console.during
    assert during["pw_wow_exec_timing"] == b"exec-timing\n" and during["pw_wow_dispatch_profile"] == b"dispatch-profile\n"
    assert during["pw_wow_timing"] == b"timing\n" and during["pw_wow_profile"] is None
    assert during[APP + "/" + run.RUNTIME] == SIGNED
    assert (root / APP / run.RUNTIME).read_bytes() == b"the app's own translator"
    for trigger in run.PROFILERS.values():
        assert not (root / trigger).exists(), trigger
    assert "profilers: exec-timing, dispatch-profile, timing, translator wowprospero.prx" in output
    assert "restored the app's own translator" in output and "removed the key script and the profiler triggers" in output
    assert (saved / "counter-strike-16-1790000000-demo.log").read_text() == "5182 frames 86.6 seconds 59.8 fps\n"

# A translator the console can't load, a wrong app folder or a missing
# --app is refused, and nothing stays changed.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    raw = Path(directory) / "raw.prx"
    raw.write_bytes(b"\x7fELF unsigned")
    eboot = Path(directory) / "eboot.bin"
    eboot.write_bytes(APP_BINARY)
    signed = Path(directory) / "signed.prx"
    signed.write_bytes(SIGNED)
    for args, message in ((["--runtime", str(raw), "--app", "/" + APP], "is not a signed module"),
                          (["--runtime", str(eboot), "--app", "/" + APP], "is not a signed module"),
                          (["--runtime", str(signed), "--app", "/data/homebrew/OTHER"], "check --app")):
        try:
            main(Console(root), "--profiler", "exec-timing", *args)
            raise AssertionError(args)
        except SystemExit as stop:
            assert message in str(stop), stop
        assert (root / APP / run.RUNTIME).read_bytes() == b"the app's own translator"
        assert not (root / "pw_wow_exec_timing").exists()
        assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
    for args in (["--runtime", str(signed)], ["--fetch", "x.log"], ["--fetch", "/abs.log", "--save", directory],
                 ["--profiler", "nosuch"]):
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                main(Console(root), *args)
            raise AssertionError(args)
        except SystemExit:
            pass

# A stopped run with another translator puts the app's own back.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    candidate = Path(directory) / "wowprospero.prx"
    candidate.write_bytes(SIGNED)
    try:
        main(Console(root, stop=True), "--runtime", str(candidate), "--app", "/" + APP, "--profiler", "profile")
        raise AssertionError("no exit")
    except SystemExit as stop:
        assert stop.code == 143
    assert (root / APP / run.RUNTIME).read_bytes() == b"the app's own translator"
    assert not (root / "pw_wow_profile").exists()

# SIGTERM or SIGHUP mid-run: the run is put back as after Ctrl-C.
try:
    run.exit_on_signal(15, None)
    raise AssertionError("no exit")
except SystemExit as stop:
    assert stop.code == 143
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    try:
        main(Console(root, stop=True), "--key", "1:enter", "--timing", "--fps")
        raise AssertionError("no exit")
    except SystemExit as stop:
        assert stop.code == 143
    assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
    assert (root / "profiles/counter-strike-16.profile").read_text() == "[application]\nid = cs16\nname = CS\n"
    assert not (root / "pw_script_keys").exists() and not (root / "pw_wow_timing").exists()

# The same signal in the middle of a transfer: the restore reconnects first.
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory) / "console"
    library(root)
    console = Console(root, stop=True, broken=True)
    try:
        main(console, "--key", "1:enter", "--timing", "--fps")
        raise AssertionError("no exit")
    except SystemExit as stop:
        assert stop.code == 143
    assert console.reconnects == 1
    assert (root / "profiles/profiles.lst").read_text() == "half-life.profile\ncounter-strike-16.profile\n"
    assert (root / "profiles/counter-strike-16.profile").read_text() == "[application]\nid = cs16\nname = CS\n"
    assert not (root / "pw_script_keys").exists() and not (root / "pw_wow_timing").exists()

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

print("pw_gameplay_run passed: keys, launch arguments, appended configs, timing trigger, fps channel and Vulkan frame rate, recorded macro, pad presses merged into it, profilers, another translator, fetched files, restore on success/timeout/refusal/signal (reconnecting after a cut transfer), stale triggers, session by profile id, summary")
