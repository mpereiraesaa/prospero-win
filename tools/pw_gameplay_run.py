#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run one game unattended on the console, past its menus, and report the run.

A script build of the title (PW_WINE64_SCRIPT=1, docs/DEBUGGING_GUIDE.md,
"Automated gameplay runs") opens the library's first game by itself, presses
the keys listed in <root>/pw_script_keys at set times, replays the macro in
<root>/pw_script_input, and closes the game after PW_WINE64_SECONDS. This tool prepares the console for one such run,
waits for it, and puts everything back:

- profiles.lst lists only the chosen game, so the script build opens it,
  and --arguments replaces the game's launch arguments for the run (a game
  that starts at its menu can start a map instead);
- pw_script_keys holds the key presses (--key MS:KEY, any number), and
  --input FILE a recorded macro of keys, mouse buttons, motion and
  controller input (<root>/pw_script_input, src/pw_script_input.h);
  --pad MS:BUTTON[:HOLDMS] adds controller button presses to that macro,
  each held HOLDMS (150 unless given), for menus that ignore keys;
- --append adds lines to files under the library root, such as a game's
  config (Counter-Strike's bot_quota), --timing turns on the WoW64
  timing report (pw_wow_timing), --profiler turns on one of the
  translator's other profilers (see PROFILERS), and --fps turns on Wine's
  fps channel in the game's profile, for a game that presents with Vulkan
  (DXVK);
- --runtime installs another build of the translator (wowprospero.prx)
  in the app's folder (--app) for the run, to compare it with the one the
  app has;
- when the run's saved session (logs/session-N.log, with profile=SLUG)
  ends, it is copied to --save and summarized: frame rate (PW_GL, or
  Wine's fps channel), the keys sent, timing lines, the CPU time each
  busy thread spent in translated code (pw_exec_cpu.py) and how the
  session ended. --fetch copies other files from the library, such as the
  game's own benchmark result, next to it.

The original profiles.lst, appended files and translator are restored, and
the key script and profiler triggers removed, even when the run fails, times out or is
stopped with SIGTERM or SIGHUP; a stale trigger an interrupted run left
behind is removed too.

Usage:
    pw_gameplay_run.py SLUG --host IP [--port N] [--remote /data/prospero-win]
        [--key MS:KEY ...] [--input FILE] [--pad MS:BUTTON[:HOLDMS] ...] [--arguments ARGS] [--append PATH=LINE ...]
        [--timing] [--profiler NAME ...] [--fps]
        [--runtime wowprospero.prx --app /data/homebrew/FOLDER]
        [--wait SECONDS] [--save DIR] [--fetch PATH ...] [--keep]
"""
from __future__ import annotations

import argparse
import ftplib
import os
import re
import signal
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pw_exec_cpu import busiest, describe, estimates  # noqa: E402
from pw_prefix import FtpRemote  # noqa: E402

# Windows virtual-key codes for the keys a menu usually needs.
KEY_NAMES = {
    "enter": 0x0D, "return": 0x0D, "escape": 0x1B, "esc": 0x1B, "space": 0x20,
    "tab": 0x09, "backspace": 0x08, "shift": 0x10, "ctrl": 0x11, "alt": 0x12,
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28,
    **{str(digit): 0x30 + digit for digit in range(10)},
    **{chr(letter): letter - 32 for letter in range(ord("a"), ord("z") + 1)},
    **{f"f{number}": 0x6F + number for number in range(1, 13)},
}
# XInput's buttons, as the macro names them (src/pw_script_input.h).
PAD_BUTTONS = {
    "up": 0x0001, "down": 0x0002, "left": 0x0004, "right": 0x0008, "start": 0x0010, "back": 0x0020,
    "ls": 0x0040, "rs": 0x0080, "lb": 0x0100, "rb": 0x0200, "guide": 0x0400,
    "a": 0x1000, "b": 0x2000, "x": 0x4000, "y": 0x8000,
}
PAD_MASK = 0xF7FF  # every button above: 0x0800 is no XInput button
PAD_HOLD_MS = 150  # long enough for a game that samples its controller once a frame
SESSIONS = 8  # native/pw_diagnostics.h: PW_DIAGNOSTICS_SESSIONS
# The translator's profilers: a file in the library root turns each on for
# the games started while it exists (wine/wowprospero/unix.c).
PROFILERS = {
    "timing": "pw_wow_timing",                      # time split: translated code, host libraries, system calls
    "exec-timing": "pw_wow_exec_timing",            # CPU time in translated code, per thread
    "profile": "pw_wow_profile",                    # each thread's busiest translated blocks
    "dispatch-profile": "pw_wow_dispatch_profile",  # chain-table hits, empty slots and collisions
}
# The run's own trigger files: never kept after a run, even one that found a
# stale copy left by an interrupted run.
TRIGGERS = ("pw_script_keys", "pw_script_input", *PROFILERS.values())
# The translator's Unix side, under the app's folder.
RUNTIME = "win/wine/lib/wine/x86_64-unix/wowprospero.prx"
# A signed module (what tools/build_wine_ps5.sh writes to prx/sce_module/);
# the console loads nothing else.
SIGNED_MODULE = bytes.fromhex("5414f5ee")
TITLE_WINEDEBUG = "err+all,+loaddll,+process"  # native/wine64_main.c: PW_WINE64_DEBUG
# Below this, a Vulkan game is still loading: its loading screen draws a
# frame now and then.
VULKAN_PLAYING_FPS = 20.0


def log(message: str) -> None:
    print(f"pw_gameplay_run: {message}", flush=True)


def parse_key(spec: str) -> tuple[int, int]:
    """'25000:enter' or '25000:0x0d' -> (milliseconds, virtual key)."""
    when, _, key = spec.partition(":")
    if not when.isdigit() or not key:
        raise ValueError(f"key {spec!r}: expected MILLISECONDS:KEY")
    key = key.lower()
    if key in KEY_NAMES:
        code = KEY_NAMES[key]
    elif re.fullmatch(r"0x[0-9a-f]{1,2}", key):
        code = int(key, 16)
    else:
        raise ValueError(f"key {spec!r}: unknown key {key!r} (a name such as enter, or 0xNN)")
    return int(when), code


def key_script(keys: list[tuple[int, int]]) -> bytes:
    """pw_script_keys: one '<ms> 0x<vk>' line per press, in time order."""
    return "".join(f"{ms} 0x{code:02x}\n" for ms, code in sorted(keys)).encode()


def parse_pad(spec: str) -> tuple[int, str, int]:
    """'45000:a', '45000:start:300' or '45000:0x1010' -> (milliseconds, button, hold milliseconds)."""
    parts = spec.split(":")
    if len(parts) not in (2, 3) or not parts[0].isdigit() or not parts[1] or \
            (len(parts) == 3 and not parts[2].isdigit()):
        raise ValueError(f"pad {spec!r}: expected MILLISECONDS:BUTTON or MILLISECONDS:BUTTON:HOLDMS")
    button = parts[1].lower()
    if button not in PAD_BUTTONS:
        mask = int(button, 16) if re.fullmatch(r"0x[0-9a-f]{1,4}", button) else 0
        if not mask or mask & ~PAD_MASK:
            raise ValueError(f"pad {spec!r}: unknown button {parts[1]!r} "
                             f"({', '.join(PAD_BUTTONS)}, or an XInput mask such as 0x1000)")
        button = f"0x{mask:04x}"
    return int(parts[0]), button, int(parts[2]) if len(parts) == 3 else PAD_HOLD_MS


def macro_script(macro: bytes | None, pads: list[tuple[int, str, int]]) -> bytes:
    """pw_script_input: the --input macro with a press and a release for each
    --pad merged in by time (at the same time, the macro's events first,
    then the presses in time order). A macro's sync line stays first; its
    comments and blank lines go."""
    if not pads:
        return macro or b""
    sync: list[str] = []
    events: list[tuple[int, str]] = []
    for number, line in enumerate((macro or b"").decode().splitlines(), 1):
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        if words[0] == "sync" and not sync and not events:
            sync.append(line.strip())
        elif words[0].isdigit():
            events.append((int(words[0]), " ".join(words)))
        else:
            raise SystemExit(f"pw_gameplay_run: --input line {number} is not a macro event: {line.strip()!r}")
    for ms, button, hold in sorted(pads):
        events += [(ms, f"{ms} pad {button} 1"), (ms + hold, f"{ms + hold} pad {button} 0")]
    events.sort(key=lambda event: event[0])  # stable: same-time events keep their order
    return "".join(f"{line}\n" for line in sync + [line for _, line in events]).encode()


def parse_append(spec: str) -> tuple[str, str]:
    path, sep, line = spec.partition("=")
    if not sep or not path or path.startswith("/") or ".." in path.split("/"):
        raise ValueError(f"append {spec!r}: expected RELATIVE/PATH=LINE under the library root")
    return path, line


def parse_fetch(path: str) -> str:
    if not path or path.startswith("/") or ".." in path.split("/"):
        raise ValueError(f"fetch {path!r}: expected a relative path under the library root")
    return path


def appended(original: bytes, lines: list[str]) -> bytes:
    """original with lines added at the end, in its own line ending."""
    newline = b"\r\n" if b"\r\n" in original else b"\n"
    body = original.rstrip(b"\r\n")
    return (body + newline if body else b"") + newline.join(line.encode() for line in lines) + newline


def with_arguments(profile: bytes, arguments: str) -> bytes:
    """The profile with its [application] arguments replaced (or added)."""
    line = f"arguments = {arguments}".encode()
    replaced, count = re.subn(rb"(?m)^[ \t]*arguments[ \t]*=.*?(?=\r?$)", lambda _: line, profile, count=1)
    if count:
        return replaced
    replaced, count = re.subn(rb"(?m)^\[application\][ \t]*\r?$", lambda match: match[0] + b"\n" + line, profile, count=1)
    if not count:
        raise SystemExit("pw_gameplay_run: the profile has no [application] section")
    return replaced


def with_fps_channel(profile: bytes) -> bytes:
    """The profile with Wine's fps channel on: added to its [debug] winedebug,
    or a [debug] section with the title's channels and +fps. A second
    winedebug line would make the profile malformed."""
    match = re.search(rb"(?m)^[ \t]*winedebug[ \t]*=[ \t]*([^\s;]+)", profile)
    if match:
        if b"+fps" in match[1].split(b","):
            return profile
        return profile[:match.end(1)] + b",+fps" + profile[match.end(1):]
    return appended(profile, ["[debug]", f"winedebug = {TITLE_WINEDEBUG},+fps"])


def session_header(text: str) -> dict[str, str]:
    first = text.split("\n", 1)[0]
    if not first.startswith("PW_REPORT/1 "):
        return {}
    return dict(field.split("=", 1) for field in first.split()[1:] if "=" in field)


def summarize(text: str) -> list[str]:
    """The run's frame rate, keys, timing and ending, from a saved session."""
    fps = [float(value) for value in re.findall(r"PW_GL frames=\d+ fps=([0-9.]+)", text)]
    # win32u's vkQueuePresentKHR, about every 1.5 s with WINEDEBUG +fps.
    vulkan = [float(value) for value in re.findall(r":trace:fps:\S+ \S+ @ approx ([0-9.]+)fps", text)]
    keys = re.findall(r"PW_WINE64 script key=(0x[0-9a-f]+) status=(\S+)", text)
    replayed = re.findall(r"PW_WINE64 script input replayed=(\d+) of (\d+)", text)
    pad = re.findall(r"PW_WINE64 script pad buttons=(\S+) at_ms=(\d+)", text)
    # Busy guest threads only: the game's, not those that only wait.
    timing = [line.split("timing: ", 1)[1] for line in text.splitlines()
              if "wowprospero timing:" in line and
              float((re.search(r" run=([0-9.]+)%", line) or [0, "0"])[1]) >= 5]
    ending = re.findall(r"session_end reason=(\S+)", text)
    macro = re.search(r"PW_WINE64 script input status=(\S+) events=(\d+) bad_line=(\d+)", text)
    lines = []
    if macro:
        done = replayed[-1][0] if replayed else "0"
        synced = "synced" if "PW_WINE64 script input synced" in text else "never synced"
        lines.append(f"macro: {macro[1]}, {done} of {macro[2]} events replayed"
                     + (f", refused at line {macro[3]}" if macro[3] != "0" else "") + f" ({synced})")
    if pad:
        lines.append(f"pad buttons held: {len(pad)} changes ("
                     + ", ".join(f"{buttons} at {ms} ms" for buttons, ms in pad[:20])
                     + (", ..." if len(pad) > 20 else "") + ")")
    if "PW_WINE64 script pad events ignored" in text:
        lines.append("pad: ignored, the game's profile does not use [input] mode = xinput")
    lines += [f"keys sent: {len(keys)}" + (f" ({', '.join(f'{code}:{status}' for code, status in keys)})" if keys else "")]
    if fps:
        # The first samples cover loading; the rest is the run itself.
        steady = fps[3:] or fps
        lines.append("fps: " + " ".join(f"{value:.1f}" for value in fps))
        lines.append(f"fps after loading: average {sum(steady) / len(steady):.1f}, "
                     f"minimum {min(steady):.1f}, at 59 or more {sum(v >= 59 for v in steady)}/{len(steady)}")
    elif vulkan:
        playing = next((i for i, value in enumerate(vulkan) if value >= VULKAN_PLAYING_FPS), len(vulkan))
        steady = vulkan[playing:]
        lines.append(f"fps (Vulkan, {len(vulkan)} samples): " + " ".join(f"{value:.1f}" for value in vulkan))
        if steady:
            lines.append(f"fps after loading: average {sum(steady) / len(steady):.1f}, "
                         f"minimum {min(steady):.1f}, at 59 or more {sum(v >= 59 for v in steady)}/{len(steady)}")
        else:
            lines.append(f"fps after loading: none (never {VULKAN_PLAYING_FPS:.0f} or more)")
    else:
        lines.append("fps: no PW_GL or Wine fps records (it never presented, or ran without --fps)")
    lines += [f"timing: {line}" for line in timing[-3:]]
    threads = estimates(text)
    if threads:
        lines.append("translated CPU: " + "; ".join(describe(thread) for thread in busiest(threads, 3)))
    lines.append(f"ended: {ending[-1] if ending else 'no session_end (the run is still going or crashed)'}")
    return lines


class Run:
    def __init__(self, args: argparse.Namespace, remote) -> None:
        self.args, self.remote = args, remote
        self.root = args.remote.rstrip("/")
        self.saved: dict[str, bytes | None] = {}
        self.runtime: tuple[str, bytes] | None = None
        self.profile_id = args.slug

    def path(self, relative: str) -> str:
        return f"{self.root}/{relative}"

    def read(self, relative: str) -> bytes | None:
        try:
            return self.remote.read(self.path(relative))
        except ftplib.all_errors:
            return None

    def replace(self, relative: str, data: bytes) -> None:
        if relative not in self.saved:
            self.saved[relative] = self.read(relative)
        self.remote.write(self.path(relative), data)

    def install_runtime(self) -> None:
        """The candidate translator in place of the app's own, for the run."""
        candidate = Path(self.args.runtime).read_bytes()
        if not candidate.startswith(SIGNED_MODULE):
            raise SystemExit(f"pw_gameplay_run: {self.args.runtime} is not a signed module: use the "
                             "prx/sce_module/wowprospero.prx that tools/build_wine_ps5.sh writes")
        target = f"{self.args.app.rstrip('/')}/{RUNTIME}"
        try:
            self.runtime = (target, self.remote.read(target))
        except ftplib.all_errors:
            raise SystemExit(f"pw_gameplay_run: no {target} on the console (check --app)")
        self.remote.write(target, candidate)

    def remove(self, relative: str) -> None:
        try:
            self.remote.delete(self.path(relative))
        except ftplib.all_errors:
            pass  # absent, or ftpsrv's 226 answer to a delete

    def prepare(self) -> None:
        slug = self.args.slug
        profile = self.read(f"profiles/{slug}.profile")
        if profile is None:
            raise SystemExit(f"pw_gameplay_run: no profiles/{slug}.profile on the console")
        # Sessions name the profile's id, which need not match its file name.
        match = re.search(rb"(?m)^\s*id\s*=\s*(\S+)", profile)
        self.profile_id = match[1].decode() if match else slug
        self.replace("profiles/profiles.lst", f"{slug}.profile\n".encode())
        changed = profile
        if self.args.arguments is not None:
            changed = with_arguments(changed, self.args.arguments)
        if self.args.fps:
            changed = with_fps_channel(changed)
        if changed != profile:
            self.replace(f"profiles/{slug}.profile", changed)
        for trigger in TRIGGERS:
            if self.read(trigger) is not None:
                log(f"removing a stale {trigger} (an interrupted run left it)")
                self.saved[trigger] = None
                self.remove(trigger)
        self.replace("pw_script_keys", key_script(self.args.keys))
        if self.args.input or self.args.pads:
            macro = Path(self.args.input).read_bytes() if self.args.input else None
            self.replace("pw_script_input", macro_script(macro, self.args.pads))
        for name in self.args.profilers:
            self.replace(PROFILERS[name], name.encode() + b"\n")
        lines: dict[str, list[str]] = {}
        for path, line in self.args.append:
            lines.setdefault(path, []).append(line)
        for path, added in lines.items():
            original = self.read(path)
            if original is None:
                raise SystemExit(f"pw_gameplay_run: {path} is not on the console")
            self.replace(path, appended(original, added))
        if self.args.runtime:
            self.install_runtime()
        log(f"prepared {slug}: {len(self.args.keys)} key presses, "
            f"{'a recorded macro, ' if self.args.input else ''}"
            f"{f'{len(self.args.pads)} pad presses, ' if self.args.pads else ''}"
            f"{sum(map(len, lines.values()))} appended lines, "
            f"profilers: {', '.join(self.args.profilers) or 'none'}"
            + (f", translator {Path(self.args.runtime).name}" if self.args.runtime else ""))

    def restore(self) -> None:
        if self.runtime:
            self.remote.write(*self.runtime)
            log("restored the app's own translator")
        for relative, original in self.saved.items():
            if original is None:
                self.remove(relative)
            else:
                self.remote.write(self.path(relative), original)
        log("restored profiles.lst and the appended files; removed the key script"
            + (" and the profiler triggers" if self.args.profilers else ""))

    def fetch(self, target: Path, stem: str) -> None:
        """Copies of --fetch files, read after the game ended."""
        for relative in self.args.fetch:
            data = self.read(relative)
            if data is None:
                log(f"could not fetch {relative}: not on the console")
                continue
            copy = target / f"{stem}-{Path(relative).name}"
            copy.write_bytes(data)
            log(f"saved {copy}")

    def next_session(self) -> int:
        data = self.read("logs/next.txt")
        return int(data.strip()) % SESSIONS if data and data.strip().isdigit() else 0

    def wait(self, first: int) -> str | None:
        """The text of the first finished session of SLUG from index first on."""
        deadline = time.monotonic() + self.args.wait
        while time.monotonic() < deadline:
            current = self.next_session()
            index = first
            while index != current:
                text = (self.read(f"logs/session-{index}.log") or b"").decode("utf-8", "replace")
                if session_header(text).get("profile") == self.profile_id and "session_end reason=" in text:
                    previous = self.read(f"logs/session-{index}.previous.log")
                    return (previous.decode("utf-8", "replace") if previous else "") + text
                index = (index + 1) % SESSIONS
            time.sleep(self.args.poll)
        return None


def main(argv: list[str] | None = None, remote=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("slug", help="the game's profile name, without .profile")
    parser.add_argument("--host", default=os.environ.get("PS5_HOST", ""))
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--remote", default="/data/prospero-win", help="the library root on the console")
    parser.add_argument("--key", dest="keys", action="append", default=[], type=parse_key,
                        help="MS:KEY, a press MS milliseconds after the game starts (enter, esc, 2, f1, 0x0d...)")
    parser.add_argument("--input", help="a recorded macro (src/pw_script_input.h) the script build replays")
    parser.add_argument("--pad", dest="pads", action="append", default=[], type=parse_pad,
                        help="MS:BUTTON[:HOLDMS], a controller button held HOLDMS (default 150) from MS "
                             "milliseconds (a b x y start back lb rb ls rs up down left right guide, or 0xNNNN); "
                             "joins --input's macro, timed like it; needs [input] mode = xinput")
    parser.add_argument("--arguments", help="the game's launch arguments for this run, in place of its profile's")
    parser.add_argument("--append", action="append", default=[], type=parse_append,
                        help="PATH=LINE, a line added to a file under the library root for the run")
    parser.add_argument("--timing", action="store_true", help="turn on the WoW64 timing report (--profiler timing)")
    parser.add_argument("--profiler", dest="profilers", action="append", default=[], choices=sorted(PROFILERS),
                        help="turn on one of the translator's profilers for the run (any number)")
    parser.add_argument("--runtime", help="a signed wowprospero.prx to run the game with, in place of the app's")
    parser.add_argument("--app", help="the app's folder on the console, for --runtime: /data/homebrew/ and its name")
    parser.add_argument("--fetch", action="append", default=[], type=parse_fetch,
                        help="PATH under the library root to copy into --save after the run (any number)")
    parser.add_argument("--fps", action="store_true",
                        help="turn on Wine's fps channel in the profile, to report a Vulkan (DXVK) game's frame rate")
    parser.add_argument("--wait", type=int, default=600, help="seconds to wait for the run (default 600)")
    parser.add_argument("--poll", type=float, default=5.0, help=argparse.SUPPRESS)
    parser.add_argument("--save", help="directory to copy the run's saved session log into")
    parser.add_argument("--keep", action="store_true", help="leave the run's settings on the console")
    args = parser.parse_args(argv)
    if args.timing and "timing" not in args.profilers:
        args.profilers.append("timing")
    if args.runtime and not args.app:
        parser.error("--runtime needs --app, the app's folder on the console")
    if args.fetch and not args.save:
        parser.error("--fetch needs --save")
    if remote is None:
        if not args.host:
            parser.error("--host (or PS5_HOST) is required")
        remote = FtpRemote(args.host, args.port)
    run = Run(args, remote)
    try:
        first = run.next_session()
        run.prepare()
        log("ready: start the script build of the title on the console now "
            f"(waiting up to {args.wait} s for its {args.slug} session)")
        text = run.wait(first)
    finally:
        if not args.keep:
            run.restore()
    if text is None:
        log(f"no finished {args.slug} session within {args.wait} s")
        return 1
    if args.save:
        target = Path(args.save) / f"{args.slug}-{session_header(text).get('time', 'run')}.log"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        log(f"saved {target}")
        run.fetch(target.parent, target.stem)
    for line in summarize(text):
        log(line)
    return 0


def exit_on_signal(number, frame) -> None:
    """SIGTERM or SIGHUP ends the run like Ctrl-C, through the restore."""
    raise SystemExit(128 + number)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, exit_on_signal)
    signal.signal(signal.SIGHUP, exit_on_signal)
    sys.exit(main())
