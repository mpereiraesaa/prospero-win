#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Push a game's prefix and profile to the console, and pull what it changed.

The library on the PC (tools/pw_install.py's --library) mirrors
/data/prospero-win on the console: prefixes/<slug> and profiles/<slug>.profile.
The console is reached through its FTP server (ps5-payload-dev/ftpsrv). A
manifest per game, LIBRARY/.pw/<slug>.json, records each file's size and
SHA-256 as last pushed or pulled, so a push sends only what changed and a
pull fetches only what the console changed.

The console's prefix is the game's live state: games keep settings in the
registry and saves beside themselves. A push refuses to overwrite a console
prefix it does not know, or one whose registry changed since the last push
or pull, until it is pulled (or --force).

A title cannot make symbolic links, so prospero-win keeps them per directory
in a .pw-symlinks table (NAME<TAB>TARGET lines, wine/ps5/pw_wine_cwd.h). A
push writes each directory's links inside the prefix there (dosdevices' c:
and z:), and links that leave the prefix (Wine's Desktop or Documents into
the PC's home) become empty directories.

Usage:
    pw_prefix.py push|pull|status SLUG --library DIR [--host IP] [--port N]
        [--remote /data/prospero-win] [--force] [--delete]
"""
from __future__ import annotations

import argparse
import ftplib
import hashlib
import io
import json
import os
import posixpath
import sys
from pathlib import Path

LINK_TABLE = ".pw-symlinks"
SKIP = {".wineserver", LINK_TABLE}
REGISTRY = ("system.reg", "user.reg", "userdef.reg")
# The one setting the console and the PC must differ in: WoW64's i386 CPU.
# The PC's Wine uses wow64cpu.dll; on the console 32-bit mode is refused and
# prospero-win's DBT, wowprospero.dll, takes its place (WINE_PS5_BUILD.md).
# A push writes the console's value, a pull the PC's, so one prefix runs on
# both; the manifest records the console's bytes.
CPU_KEY = b"[Software\\\\Microsoft\\\\Wow64\\\\x86]"
PC_CPU, CONSOLE_CPU = b'@="wow64cpu.dll"', b'@="wowprospero.dll"'


def swap_cpu(key: str, data: bytes, old: bytes, new: bytes) -> bytes:
    """system.reg with the Wow64\\x86 default changed from old to new."""
    if key != "system.reg":
        return data
    start = data.find(CPU_KEY)
    if start < 0:
        return data
    end = data.find(b"\n[", start + 1)
    end = len(data) if end < 0 else end
    return data[:start] + data[start:end].replace(old, new) + data[end:]


def to_console(key: str, data: bytes) -> bytes:
    return swap_cpu(key, data, PC_CPU, CONSOLE_CPU)


def to_pc(key: str, data: bytes) -> bytes:
    return swap_cpu(key, data, CONSOLE_CPU, PC_CPU)


class SyncError(Exception):
    pass


def log(message: str) -> None:
    print(f"pw_prefix: {message}", flush=True)


class FtpRemote:
    """The console's ftpsrv: MLSD lists the current directory only."""

    def __init__(self, host: str, port: int):
        self.ftp = ftplib.FTP()
        self.ftp.connect(host, port, 30)
        self.ftp.login()
        # ftpsrv converts SELF containers on the fly unless told not to.
        for _ in range(2):
            if "disabled" in self.ftp.sendcmd("SELF").lower():
                break

    def listdir(self, path: str) -> dict[str, tuple[str, int]]:
        self.ftp.cwd(path)
        return {name: (facts.get("type", "file"), int(facts.get("size", 0)))
                for name, facts in self.ftp.mlsd() if name not in (".", "..")}

    def exists(self, path: str) -> bool:
        try:
            self.ftp.cwd(path)
            return True
        except ftplib.error_perm:
            try:
                self.ftp.size(path)
                return True
            except ftplib.error_perm:
                return False

    def size(self, path: str) -> int | None:
        try:
            return self.ftp.size(path)
        except ftplib.error_perm:
            return None

    def makedirs(self, path: str) -> None:
        parts = path.strip("/").split("/")
        for index in range(1, len(parts) + 1):
            try:
                self.ftp.mkd("/" + "/".join(parts[:index]))
            except ftplib.error_perm:
                pass

    def write(self, path: str, data: bytes) -> None:
        self.ftp.storbinary(f"STOR {path}", io.BytesIO(data))

    def read(self, path: str) -> bytes:
        out = io.BytesIO()
        self.ftp.retrbinary(f"RETR {path}", out.write)
        return out.getvalue()

    def delete(self, path: str) -> None:
        self.ftp.delete(path)

    def close(self) -> None:
        self.ftp.quit()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def local_tree(prefix: Path) -> tuple[dict[str, Path], set[str], dict[str, dict[str, str]]]:
    """The prefix's files, directories and per-directory link tables."""
    files, dirs, links = {}, {""}, {}
    root = prefix.resolve()
    pending = [""]
    while pending:
        relative = pending.pop()
        for entry in sorted(os.scandir(prefix / relative), key=lambda e: e.name):
            key = posixpath.join(relative, entry.name) if relative else entry.name
            if entry.name in SKIP:
                continue
            if entry.is_symlink():
                target = os.readlink(entry.path)
                if target.startswith("/dev"):
                    continue    # serial and parallel ports: nothing on a console
                inside = target == "/" or (not target.startswith("/") and
                                           (Path(entry.path).parent / target).resolve().is_relative_to(root))
                if inside:
                    links.setdefault(relative, {})[entry.name] = target
                else:
                    dirs.add(key)   # a link out of the prefix: an empty directory
            elif entry.is_dir():
                dirs.add(key)
                pending.append(key)
            elif entry.is_file():
                files[key] = Path(entry.path)
    return files, dirs, links


class Sync:
    def __init__(self, args: argparse.Namespace, remote=None):
        self.slug = args.slug
        self.library = Path(args.library).resolve()
        self.prefix = self.library / "prefixes" / self.slug
        self.profile = self.library / "profiles" / f"{self.slug}.profile"
        self.remote_root = args.remote.rstrip("/")
        self.remote_prefix = f"{self.remote_root}/prefixes/{self.slug}"
        self.manifest_path = self.library / ".pw" / f"{self.slug}.json"
        self.manifest = json.loads(self.manifest_path.read_text()) if self.manifest_path.is_file() else None
        self.remote = remote or FtpRemote(args.host, args.port)
        self.force, self.delete = args.force, getattr(args, "delete", False)

    def save(self, files: dict[str, list]) -> None:
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        self.manifest = {"slug": self.slug, "files": files}
        self.manifest_path.write_text(json.dumps(self.manifest, indent=1, sort_keys=True))

    def registry_drift(self) -> list[str]:
        """Registry files the console changed since the manifest."""
        known = (self.manifest or {}).get("files", {})
        drift = []
        for name in REGISTRY:
            size = self.remote.size(f"{self.remote_prefix}/{name}")
            if size is not None and (name not in known or known[name][0] != size):
                drift.append(name)
        return drift

    def push(self) -> None:
        if not self.prefix.is_dir() or not self.profile.is_file():
            raise SyncError(f"no installed {self.slug} in {self.library} (tools/pw_install.py)")
        if self.remote.exists(self.remote_prefix) and not self.force:
            if self.manifest is None:
                raise SyncError(f"{self.remote_prefix} exists and was never pulled here: pull it first, or --force")
            drift = self.registry_drift()
            if drift:
                raise SyncError(f"the console changed {', '.join(drift)} since the last sync: pull first, or --force")
        known = (self.manifest or {}).get("files", {})
        files, dirs, links = local_tree(self.prefix)
        for directory in sorted(dirs):
            self.remote.makedirs(posixpath.join(self.remote_prefix, directory) if directory else self.remote_prefix)
        pushed, sent = {}, 0
        for key, path in sorted(files.items()):
            data = to_console(key, path.read_bytes())
            entry = [len(data), sha256(data)]
            pushed[key] = entry
            if known.get(key) == entry:
                continue
            target = f"{self.remote_prefix}/{key}"
            self.remote.write(target, data)
            if self.remote.size(target) != len(data):
                raise SyncError(f"{target}: the console stored a different size")
            sent += 1
        for directory, table in sorted(links.items()):
            text = "".join(f"{name}\t{target}\n" for name, target in sorted(table.items())).encode()
            where = posixpath.join(self.remote_prefix, directory, LINK_TABLE) if directory else f"{self.remote_prefix}/{LINK_TABLE}"
            self.remote.write(where, text)
        removed = 0
        if self.delete:
            for key in sorted(set(known) - set(files)):
                self.remote.delete(f"{self.remote_prefix}/{key}")
                removed += 1
        self.push_profile()
        self.save(pushed)
        log(f"pushed {self.slug}: {sent} of {len(files)} files sent, {removed} removed, "
            f"{sum(len(t) for t in links.values())} links in {len(links)} tables")

    def push_profile(self) -> None:
        profiles = f"{self.remote_root}/profiles"
        self.remote.makedirs(profiles)
        self.remote.write(f"{profiles}/{self.slug}.profile", self.profile.read_bytes())
        # The launcher lists profiles.lst's order when it exists.
        listing = f"{profiles}/profiles.lst"
        if self.remote.size(listing) is not None:
            lines = self.remote.read(listing).decode("latin-1").splitlines()
            if f"{self.slug}.profile" not in (line.strip() for line in lines):
                self.remote.write(listing, ("\n".join(lines + [f"{self.slug}.profile"]) + "\n").encode("latin-1"))

    def walk_remote(self, directory: str = "") -> dict[str, int]:
        found = {}
        base = posixpath.join(self.remote_prefix, directory) if directory else self.remote_prefix
        for name, (kind, size) in self.remote.listdir(base).items():
            key = posixpath.join(directory, name) if directory else name
            if name in SKIP:
                continue
            if kind == "dir":
                found.update(self.walk_remote(key))
            else:
                found[key] = size
        return found

    def pull(self) -> None:
        if not self.remote.exists(self.remote_prefix):
            raise SyncError(f"the console has no {self.remote_prefix}")
        known = (self.manifest or {}).get("files", {})
        remote = self.walk_remote()
        pulled, fetched = {}, 0
        for key, size in sorted(remote.items()):
            local = self.prefix / key
            if key in known and known[key][0] == size and key not in REGISTRY and local.is_file():
                pulled[key] = known[key]
                continue
            data = self.remote.read(f"{self.remote_prefix}/{key}")
            entry = [len(data), sha256(data)]
            if not local.is_file() or sha256(to_console(key, local.read_bytes())) != entry[1]:
                local.parent.mkdir(parents=True, exist_ok=True)
                local.write_bytes(to_pc(key, data))
                fetched += 1
            pulled[key] = entry
        self.save(pulled)
        log(f"pulled {self.slug}: {fetched} of {len(remote)} files changed on the console")

    def status(self) -> None:
        known = (self.manifest or {}).get("files", {})
        files, _, _ = local_tree(self.prefix) if self.prefix.is_dir() else ({}, set(), {})
        changed = [key for key, path in files.items()
                   if known.get(key, [None, None])[1] != sha256(to_console(key, path.read_bytes()))]
        log(f"{self.slug}: {len(files)} local files, {len(changed)} not on the console yet; "
            f"console registry changed: {', '.join(self.registry_drift()) or 'no'}")


def main(argv: list[str] | None = None, remote=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("push", "pull", "status"))
    parser.add_argument("slug")
    parser.add_argument("--library", required=True)
    parser.add_argument("--host", default=os.environ.get("PS5_HOST", ""))
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--remote", default="/data/prospero-win")
    parser.add_argument("--force", action="store_true", help="overwrite what the console changed")
    parser.add_argument("--delete", action="store_true", help="remove files the PC no longer has")
    args = parser.parse_args(argv)
    if remote is None and not args.host:
        print("pw_prefix: give --host or PS5_HOST", file=sys.stderr)
        return 2
    try:
        sync = Sync(args, remote)
        getattr(sync, args.command)()
    except (SyncError, OSError, *ftplib.all_errors) as error:
        print(f"pw_prefix: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
