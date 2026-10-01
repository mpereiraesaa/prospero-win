#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run one game unattended on the console, past its menus, and report the run.

A script build of the title (PW_WINE64_SCRIPT=1, docs/DEBUGGING_GUIDE.md,
"Automated gameplay runs") opens the library's first game by itself, presses
the keys listed in <root>/pw_script_keys at set times, and closes the game
after PW_WINE64_SECONDS. This tool prepares the console for one such run,
waits for it, and puts everything back:

- profiles.lst lists only the chosen game, so the script build opens it,
  and --arguments replaces the game's launch arguments for the run (a game
  that starts at its menu can start a map instead);
- pw_script_keys holds the key presses (--key MS:KEY, any number);
- --append adds lines to files under the library root, such as a game's
  config (Counter-Strike's bot_quota), and --timing turns on the WoW64
  timing report (pw_wow_timing);
- when the run's saved session (logs/session-N.log, with profile=SLUG)
  ends, it is copied to --save and summarized: frame rate (PW_GL), the
  keys sent, timing lines and how the session ended.

The original profiles.lst and appended files are restored, and the key
script and timing trigger removed, even when the run fails or times out.

Usage:
    pw_gameplay_run.py SLUG --host IP [--port N] [--remote /data/prospero-win]
        [--key MS:KEY ...] [--arguments ARGS] [--append PATH=LINE ...] [--timing]
        [--wait SECONDS] [--save DIR] [--keep]
"""
from __future__ import annotations

import argparse
import ftplib
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
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
SESSIONS = 8  # native/pw_diagnostics.h: PW_DIAGNOSTICS_SESSIONS


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


def parse_append(spec: str) -> tuple[str, str]:
    path, sep, line = spec.partition("=")
    if not sep or not path or path.startswith("/") or ".." in path.split("/"):
        raise ValueError(f"append {spec!r}: expected RELATIVE/PATH=LINE under the library root")
    return path, line


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


def session_header(text: str) -> dict[str, str]:
    first = text.split("\n", 1)[0]
    if not first.startswith("PW_REPORT/1 "):
        return {}
    return dict(field.split("=", 1) for field in first.split()[1:] if "=" in field)


def summarize(text: str) -> list[str]:
    """The run's frame rate, keys, timing and ending, from a saved session."""
    fps = [float(value) for value in re.findall(r"PW_GL frames=\d+ fps=([0-9.]+)", text)]
    keys = re.findall(r"PW_WINE64 script key=(0x[0-9a-f]+) status=(\S+)", text)
    # Busy guest threads only: the game's, not those that only wait.
    timing = [line.split("timing: ", 1)[1] for line in text.splitlines()
              if "wowprospero timing:" in line and
              float((re.search(r" run=([0-9.]+)%", line) or [0, "0"])[1]) >= 5]
    ending = re.findall(r"session_end reason=(\S+)", text)
    lines = [f"keys sent: {len(keys)}" + (f" ({', '.join(f'{code}:{status}' for code, status in keys)})" if keys else "")]
    if fps:
        # The first samples cover loading; the rest is the run itself.
        steady = fps[3:] or fps
        lines.append("fps: " + " ".join(f"{value:.1f}" for value in fps))
        lines.append(f"fps after loading: average {sum(steady) / len(steady):.1f}, "
                     f"minimum {min(steady):.1f}, at 59 or more {sum(v >= 59 for v in steady)}/{len(steady)}")
    else:
        lines.append("fps: no PW_GL records (not an OpenGL game, or it never presented)")
    lines += [f"timing: {line}" for line in timing[-3:]]
    lines.append(f"ended: {ending[-1] if ending else 'no session_end (the run is still going or crashed)'}")
    return lines


class Run:
    def __init__(self, args: argparse.Namespace, remote) -> None:
        self.args, self.remote = args, remote
        self.root = args.remote.rstrip("/")
        self.saved: dict[str, bytes | None] = {}
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
        if self.args.arguments is not None:
            self.replace(f"profiles/{slug}.profile", with_arguments(profile, self.args.arguments))
        self.replace("pw_script_keys", key_script(self.args.keys))
        if self.args.timing:
            self.replace("pw_wow_timing", b"timing\n")
        lines: dict[str, list[str]] = {}
        for path, line in self.args.append:
            lines.setdefault(path, []).append(line)
        for path, added in lines.items():
            original = self.read(path)
            if original is None:
                raise SystemExit(f"pw_gameplay_run: {path} is not on the console")
            self.replace(path, appended(original, added))
        log(f"prepared {slug}: {len(self.args.keys)} key presses, "
            f"{sum(map(len, lines.values()))} appended lines, timing {'on' if self.args.timing else 'off'}")

    def restore(self) -> None:
        for relative, original in self.saved.items():
            if original is None:
                self.remove(relative)
            else:
                self.remote.write(self.path(relative), original)
        log("restored profiles.lst and the appended files; removed the key script"
            + (" and the timing trigger" if self.args.timing else ""))

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
    parser.add_argument("--arguments", help="the game's launch arguments for this run, in place of its profile's")
    parser.add_argument("--append", action="append", default=[], type=parse_append,
                        help="PATH=LINE, a line added to a file under the library root for the run")
    parser.add_argument("--timing", action="store_true", help="turn on the WoW64 timing report for the run")
    parser.add_argument("--wait", type=int, default=600, help="seconds to wait for the run (default 600)")
    parser.add_argument("--poll", type=float, default=5.0, help=argparse.SUPPRESS)
    parser.add_argument("--save", help="directory to copy the run's saved session log into")
    parser.add_argument("--keep", action="store_true", help="leave the run's settings on the console")
    args = parser.parse_args(argv)
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
    for line in summarize(text):
        log(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
