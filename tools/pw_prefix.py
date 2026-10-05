#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Push a game's prefix and profile to the console, and pull what it changed.

The library on the PC (tools/pw_install.py's --library) mirrors
/data/prospero-win on the console: prefixes/<slug> and profiles/<slug>.profile.
The console is reached through its FTP server (ps5-payload-dev/ftpsrv). A
manifest per game, LIBRARY/.pw/<slug>.json, records each file's size and
SHA-256 as last pushed or pulled, so a push sends only what changed and a
pull fetches only what the console changed.

Files are read, hashed and sent in chunks of CHUNK bytes, so a game's
largest archive costs no more memory than its smallest. Only the registry
files, which a push and a pull rewrite (to_console, to_pc), are held whole.
A push saves the manifest as it goes (every SAVE_EVERY_FILES files or
SAVE_EVERY_BYTES bytes, and when interrupted), so a push that stops part way
resumes where it stopped.

The console's prefix is the game's live state: games keep settings in the
registry and saves beside themselves. A push refuses to overwrite a console
prefix it does not know, or one whose registry changed since the last push
or pull, until it is pulled (or --force).

A title cannot make symbolic links, so prospero-win keeps them per directory
in a .pw-symlinks table (NAME<TAB>TARGET lines, wine/ps5/pw_wine_cwd.h). A
push writes each directory's links inside the prefix there (dosdevices' c:
and z:), and links that leave the prefix (Wine's Desktop or Documents into
the PC's home) become empty directories. A pull never writes through such
a link: it replaces the link with a real directory inside the prefix, as on
the console, and refuses a file that would still land outside the prefix.

--transport ps5upload sends the game's files through ps5upload's payload
(github.com/phantomptr/ps5upload) instead of FTP, using its
ps5upload-lab client. The registry, the link tables, wowprospero.dll and the
profile still go over FTP, which also checks every file's size afterwards.

Usage:
    pw_prefix.py push|pull|status SLUG --library DIR [--host IP] [--port N]
        [--remote /data/prospero-win] [--force] [--delete] [--trust-size]
        [--transport ftp|ps5upload] [--ps5upload-client PATH]
"""
from __future__ import annotations

import argparse
import ftplib
import hashlib
import io
import json
import os
import posixpath
import re
import secrets
import shutil
import signal
import subprocess
import sys
import time
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
# WoW64 loads its CPU from the prefix's system32 and does not fall back to
# the runtime's copy (c0000135 without it), so a push puts the console's
# there: --cpu-dll, tools/build_wowprospero.sh's output.
CPU_DLL = "drive_c/windows/system32/wowprospero.dll"
# The title runs Wine as USER=prospero (native/wine64_main.c), so on the
# console Windows' user folders are C:\users\prospero. tools/pw_install.py
# makes prefixes as that user; a prefix made another way has only its own
# user's folders, and the console cannot make them (wineboot does not run
# there). Games then fail to save settings under AppData, and lose them at
# exit, so a push makes these folders on the console when the prefix lacks
# them.
CONSOLE_USER = "prospero"
CONSOLE_USER_DIRS = ("AppData/Local/Temp", "AppData/LocalLow", "AppData/Roaming",
                     "Desktop", "Documents", "Saved Games")
# The most a file read, hash or FTP block holds at once.
CHUNK = 4 << 20
# How often a push records its progress in the manifest.
SAVE_EVERY_FILES = 200
SAVE_EVERY_BYTES = 256 << 20
# ps5upload's payload: transactions on 9113; hello and query-tx on its
# management port, 9114 (it answers wrong_port to anything else).
# Each transaction carries up to PS5UPLOAD_BATCH_* of the game, and a push
# records a batch once the console committed it.
PS5UPLOAD_PORT, PS5UPLOAD_MGMT_PORT = 9113, 9114
PS5UPLOAD_BATCH_FILES = 2000
PS5UPLOAD_BATCH_BYTES = 4 << 30
# How long to ask after a commit the client stopped waiting for.
PS5UPLOAD_COMMIT_WAIT, PS5UPLOAD_POLL = 900, 5


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


class Interrupted(Exception):
    """SIGTERM, raised where the push is so it can record its progress."""


def log(message: str) -> None:
    print(f"pw_prefix: {message}", flush=True)


def gib(size: int) -> str:
    return f"{size / (1 << 30):.2f} GiB"


class FtpRemote:
    """The console's ftpsrv: MLSD lists the current directory only."""

    def __init__(self, host: str, port: int):
        self.host, self.port = host, port
        self.ftp = ftplib.FTP()
        self.ftp.connect(host, port, 30)
        self.ftp.login()
        self.made: set[str] = set()
        # ftpsrv converts SELF containers on the fly unless told not to. A
        # server without the command (zftpd answers 500) converts nothing.
        for _ in range(2):
            try:
                reply = self.ftp.sendcmd("SELF")
            except ftplib.error_perm:
                break
            if "disabled" in reply.lower():
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
            directory = "/" + "/".join(parts[:index])
            if directory in self.made:
                continue
            try:
                self.ftp.mkd(directory)
            except ftplib.error_perm:
                pass
            self.made.add(directory)

    def write(self, path: str, data: bytes) -> None:
        self.ftp.storbinary(f"STOR {path}", io.BytesIO(data))

    def write_stream(self, path: str, stream) -> None:
        """Stores what stream.read() returns, CHUNK bytes at a time."""
        self.ftp.storbinary(f"STOR {path}", stream, blocksize=CHUNK)

    def read(self, path: str) -> bytes:
        out = io.BytesIO()
        self.ftp.retrbinary(f"RETR {path}", out.write)
        return out.getvalue()

    def read_stream(self, path: str, take) -> None:
        """Calls take with the file's bytes, CHUNK at most at a time."""
        self.ftp.retrbinary(f"RETR {path}", take, blocksize=CHUNK)

    def delete(self, path: str) -> None:
        # ftpsrv confirms a delete with 226, which ftplib's delete() refuses.
        self.ftp.sendcmd(f"DELE {path}")

    def close(self) -> None:
        self.ftp.quit()

    def reconnect(self) -> None:
        """A new control connection, for when a transfer was cut off half-way
        (a signal during a read leaves the old one out of step with the
        server's replies)."""
        try:
            self.ftp.close()
        except (OSError, EOFError):
            pass
        self.__init__(self.host, self.port)


class HashingReader:
    """A file read at most CHUNK bytes at a time, hashed and counted on the way."""

    def __init__(self, stream):
        self.stream, self.hash, self.size = stream, hashlib.sha256(), 0

    def read(self, size: int = -1) -> bytes:
        data = self.stream.read(CHUNK if size is None or size < 0 or size > CHUNK else size)
        self.hash.update(data)
        self.size += len(data)
        return data

    def entry(self) -> list:
        return [self.size, self.hash.hexdigest()]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_entry(key: str, path: Path) -> list:
    """[size, SHA-256] of the file as the console has it."""
    if key in REGISTRY:
        data = to_console(key, path.read_bytes())
        return [len(data), sha256(data)]
    with path.open("rb") as stream:
        reader = HashingReader(stream)
        while reader.read(CHUNK):
            pass
    return reader.entry()


def link_inside(link: Path, target: str, root: Path) -> bool:
    """Whether push keeps link in a .pw-symlinks table rather than making it
    an empty directory: z:'s "/" and links that stay inside the prefix."""
    return target == "/" or (not target.startswith("/") and
                             (link.parent / target).resolve().is_relative_to(root))


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
                if link_inside(Path(entry.path), target, root):
                    links.setdefault(relative, {})[entry.name] = target
                else:
                    dirs.add(key)   # a link out of the prefix: an empty directory
            elif entry.is_dir():
                dirs.add(key)
                pending.append(key)
            elif entry.is_file():
                files[key] = Path(entry.path)
    return files, dirs, links


class Ps5Upload:
    """ps5upload's payload on the console, driven through its ps5upload-lab client."""

    def __init__(self, client: str, host: str):
        self.client, self.host = client, host

    def call(self, port: int, *args: str, timeout: float | None = None) -> tuple[int, str]:
        try:
            done = subprocess.run([self.client, f"{self.host}:{port}", *args], capture_output=True,
                                  text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return -1, f"no answer in {timeout} s"
        return done.returncode, done.stdout + done.stderr

    def check(self) -> None:
        code, output = self.call(PS5UPLOAD_MGMT_PORT, "hello", timeout=15)
        if code != 0:
            raise SyncError(f"ps5upload's payload does not answer on {self.host}:{PS5UPLOAD_MGMT_PORT} "
                            f"({output.strip().splitlines()[-1] if output.strip() else f'exit {code}'}): "
                            "send ps5upload.elf to the console first, or use --transport ftp")

    @staticmethod
    def state(output: str) -> str | None:
        found = re.findall(r'"state"\s*:\s*"(\w+)"', output)
        return found[-1] if found else None

    def send_dir(self, tx_id: str, dest: str, source: Path) -> None:
        """Sends source's files under dest, in one transaction, and waits for its commit."""
        code, output = self.call(PS5UPLOAD_PORT, "transfer-dir", tx_id, dest, str(source))
        if code == 0 and self.state(output) == "committed":
            return
        # A large batch can outlast the client's wait for the commit while
        # the payload still applies it: ask the management port.
        deadline = time.monotonic() + PS5UPLOAD_COMMIT_WAIT
        while True:
            _, answer = self.call(PS5UPLOAD_MGMT_PORT, "query-tx", tx_id, timeout=30)
            state = self.state(answer)
            if state == "committed":
                return
            if state != "active" or time.monotonic() > deadline:
                detail = (output.strip().splitlines() or [f"exit {code}"])[-1]
                raise SyncError(f"ps5upload transaction {tx_id} ended {state or 'unknown'}: {detail}")
            time.sleep(PS5UPLOAD_POLL)


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
        self.force, self.delete = args.force, getattr(args, "delete", False)
        self.trust_size = getattr(args, "trust_size", False)
        self.cpu_dll = Path(args.cpu_dll) if getattr(args, "cpu_dll", None) else None
        self.ps5upload = None
        if getattr(args, "transport", "ftp") == "ps5upload" and args.command == "push":
            client = args.ps5upload_client or os.environ.get("PS5UPLOAD_LAB") or shutil.which("ps5upload-lab")
            if not client or not os.access(client, os.X_OK):
                raise SyncError(f"no ps5upload client{f' at {client}' if client else ''}: give --ps5upload-client, "
                                "ps5upload's engine/target/release/ps5upload-lab")
            self.ps5upload = Ps5Upload(client, args.host)
            self.ps5upload.check()
        self.remote = remote or FtpRemote(args.host, args.port)
        # What the console has, as the push goes: the manifest it saves.
        self.progress: dict[str, list] | None = None
        self.unsaved_files = self.unsaved_bytes = 0

    def save(self, files: dict[str, list]) -> None:
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        self.manifest = {"slug": self.slug, "files": files}
        # Replaced whole, so a push killed while saving keeps the last one.
        partial = self.manifest_path.with_name(self.manifest_path.name + ".tmp")
        partial.write_text(json.dumps(self.manifest, indent=1, sort_keys=True))
        os.replace(partial, self.manifest_path)

    def record(self, key: str, entry: list, sent: int = 0) -> None:
        """The console has entry for key: saved now and then, so a push resumes."""
        self.progress[key] = entry
        self.unsaved_files += 1
        self.unsaved_bytes += sent
        if self.unsaved_files >= SAVE_EVERY_FILES or self.unsaved_bytes >= SAVE_EVERY_BYTES:
            self.checkpoint()

    def checkpoint(self) -> None:
        if self.progress is not None and self.unsaved_files:
            self.save(dict(self.progress))
            self.unsaved_files = self.unsaved_bytes = 0

    def registry_drift(self) -> list[str]:
        """Registry files the console changed since the manifest."""
        known = (self.manifest or {}).get("files", {})
        drift = []
        for name in REGISTRY:
            size = self.remote.size(f"{self.remote_prefix}/{name}")
            if size is not None and (name not in known or known[name][0] != size):
                drift.append(name)
        return drift

    def send(self, key: str, path: Path) -> list:
        """Sends one file over FTP; [size, SHA-256] of what the console stored."""
        target = f"{self.remote_prefix}/{key}"
        if key in REGISTRY:
            data = to_console(key, path.read_bytes())
            self.remote.write(target, data)
            entry = [len(data), sha256(data)]
        else:
            with path.open("rb") as stream:
                reader = HashingReader(stream)
                self.remote.write_stream(target, reader)
            entry = reader.entry()
        if self.remote.size(target) != entry[0]:
            raise SyncError(f"{target}: the console stored a different size")
        return entry

    def push(self) -> None:
        if not self.prefix.is_dir() or not self.profile.is_file():
            raise SyncError(f"no installed {self.slug} in {self.library} (tools/pw_install.py)")
        exists = self.remote.exists(self.remote_prefix)
        if exists and not self.force:
            if self.manifest is None:
                raise SyncError(f"{self.remote_prefix} exists and was never pulled here: pull it first, or --force")
            drift = self.registry_drift()
            if drift:
                raise SyncError(f"the console changed {', '.join(drift)} since the last sync: pull first, or --force")
        known = (self.manifest or {}).get("files", {})
        files, dirs, links = local_tree(self.prefix)
        if self.cpu_dll:
            if not self.cpu_dll.is_file():
                raise SyncError(f"--cpu-dll {self.cpu_dll} does not exist")
            files[CPU_DLL] = self.cpu_dll
            dirs.add(posixpath.dirname(CPU_DLL))
        elif CPU_DLL not in files and CPU_DLL not in known and \
                self.remote.size(f"{self.remote_prefix}/{CPU_DLL}") is None:
            raise SyncError("the console needs wowprospero.dll in the prefix: give --cpu-dll "
                            "(tools/build_wowprospero.sh's x86_64-windows/wowprospero.dll)")
        # --trust-size: a file the manifest does not know but the console has
        # at the same size is taken as already there, without comparing bytes.
        console = self.walk_remote() if self.trust_size and exists else {}
        for directory in sorted(dirs):
            self.remote.makedirs(posixpath.join(self.remote_prefix, directory) if directory else self.remote_prefix)
        users = "drive_c/users"
        if (self.prefix / users).is_dir() and not (self.prefix / users / CONSOLE_USER).is_dir():
            log(f"the prefix has no {users}/{CONSOLE_USER}, the user the console runs Wine as: "
                "making its folders there, so games can save settings under AppData")
            for directory in CONSOLE_USER_DIRS:
                self.remote.makedirs(posixpath.join(self.remote_prefix, users, CONSOLE_USER, directory))
        self.progress = dict(known)
        pushed, batch = {}, []
        sent = trusted = sent_bytes = batch_bytes = 0
        try:
            for key, path in sorted(files.items()):
                old, entry, size = known.get(key), None, path.stat().st_size
                if key in REGISTRY or (old is not None and old[0] == size):
                    entry = file_entry(key, path)
                    if entry == old:
                        pushed[key] = entry
                        continue
                elif old is None and console.get(key) == size:
                    pushed[key] = file_entry(key, path)
                    self.record(key, pushed[key])
                    trusted += 1
                    continue
                if self.ps5upload and key not in REGISTRY and key != CPU_DLL:
                    batch.append((key, path, entry or file_entry(key, path)))
                    batch_bytes += batch[-1][2][0]
                    if len(batch) >= PS5UPLOAD_BATCH_FILES or batch_bytes >= PS5UPLOAD_BATCH_BYTES:
                        sent_bytes += self.send_batch(batch, pushed)
                        sent += len(batch)
                        batch, batch_bytes = [], 0
                    continue
                pushed[key] = self.send(key, path)
                self.record(key, pushed[key], pushed[key][0])
                sent += 1
                sent_bytes += pushed[key][0]
                if self.unsaved_files == 0:
                    log(f"{sent} files sent ({gib(sent_bytes)}), progress saved")
            if batch:
                sent_bytes += self.send_batch(batch, pushed)
                sent += len(batch)
        except BaseException:
            self.checkpoint()
            raise
        for directory, table in sorted(links.items()):
            text = "".join(f"{name}\t{target}\n" for name, target in sorted(table.items())).encode()
            where = posixpath.join(self.remote_prefix, directory, LINK_TABLE) if directory else f"{self.remote_prefix}/{LINK_TABLE}"
            self.remote.write(where, text)
        removed = 0
        if self.delete:
            for key in sorted(set(known) - set(files) - {CPU_DLL}):
                self.remote.delete(f"{self.remote_prefix}/{key}")
                removed += 1
        self.push_profile()
        self.save(pushed)
        log(f"pushed {self.slug}: {sent} of {len(files)} files sent ({gib(sent_bytes)}), "
            f"{trusted} taken by size, {removed} removed, "
            f"{sum(len(t) for t in links.values())} links in {len(links)} tables")

    def send_batch(self, batch: list[tuple[str, Path, list]], pushed: dict[str, list]) -> int:
        """Sends files through ps5upload, from a directory of links to them, and checks them over FTP."""
        stage = self.library / ".pw" / "stage" / self.slug
        shutil.rmtree(stage, ignore_errors=True)
        size = sum(entry[0] for _, _, entry in batch)
        log(f"sending {len(batch)} files ({gib(size)}) through ps5upload")
        try:
            for key, path, _ in batch:
                link = stage / key
                link.parent.mkdir(parents=True, exist_ok=True)
                try:
                    os.link(path, link)     # the same file, not a copy
                except OSError:
                    os.symlink(path.resolve(), link)
            self.ps5upload.send_dir(secrets.token_hex(16), self.remote_prefix, stage)
        finally:
            shutil.rmtree(stage, ignore_errors=True)
        for key, _, entry in batch:
            target = f"{self.remote_prefix}/{key}"
            if self.remote.size(target) != entry[0]:
                raise SyncError(f"{target}: the console stored a different size")
            pushed[key] = entry
            self.record(key, entry, entry[0])
        self.checkpoint()
        return size

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

    def local_path(self, key: str) -> Path | None:
        """Where a pulled file goes, never outside the prefix. A directory on
        the way that is a link out of it (Wine's Documents into the PC's
        home) is a real directory on the console: it becomes one here too,
        so the file lands in the prefix and the PC's home is left alone.
        None when the path still leads out of the prefix (through z:)."""
        root = self.prefix.resolve()
        current = self.prefix
        *parents, name = key.split("/")
        for part in parents:
            current = current / part
            if current.is_symlink():
                target = os.readlink(current)
                if not link_inside(current, target, root):
                    log(f"{current.relative_to(self.prefix)} linked out of the prefix, to {target}: "
                        "made a folder in the prefix instead, as on the console")
                    current.unlink()
                    current.mkdir()
        local = current / name
        if not local.parent.resolve().is_relative_to(root):
            return None
        return local

    def fetch(self, key: str, local: Path) -> tuple[list, bool]:
        """Brings one console file to local if it differs; its entry, and whether it did."""
        target = f"{self.remote_prefix}/{key}"
        if key in REGISTRY:
            data = self.remote.read(target)
            entry = [len(data), sha256(data)]
            if local.is_file() and file_entry(key, local)[1] == entry[1]:
                return entry, False
            local.parent.mkdir(parents=True, exist_ok=True)
            if local.is_symlink():
                local.unlink()  # the console's file, not what the link points to
            local.write_bytes(to_pc(key, data))
            return entry, True
        partial = self.manifest_path.with_name(f"{self.slug}.pull")
        partial.parent.mkdir(parents=True, exist_ok=True)
        try:
            with partial.open("wb") as out:
                digest, size = hashlib.sha256(), 0

                def take(chunk: bytes) -> None:
                    nonlocal size
                    digest.update(chunk)
                    size += len(chunk)
                    out.write(chunk)
                self.remote.read_stream(target, take)
            entry = [size, digest.hexdigest()]
            if local.is_file() and file_entry(key, local) == entry:
                return entry, False
            local.parent.mkdir(parents=True, exist_ok=True)
            os.replace(partial, local)
            return entry, True
        finally:
            partial.unlink(missing_ok=True)

    def pull(self) -> None:
        if not self.remote.exists(self.remote_prefix):
            raise SyncError(f"the console has no {self.remote_prefix}")
        known = (self.manifest or {}).get("files", {})
        remote = self.walk_remote()
        pulled, fetched, refused = {}, 0, []
        for key, size in sorted(remote.items()):
            local = self.local_path(key)
            if local is None:
                refused.append(key)
                continue
            if key in known and known[key][0] == size and key not in REGISTRY and local.is_file():
                pulled[key] = known[key]
                continue
            pulled[key], changed = self.fetch(key, local)
            fetched += changed
        self.save(pulled)
        log(f"pulled {self.slug}: {fetched} of {len(remote)} files changed on the console")
        if refused:
            raise SyncError(f"not pulled, as they would be written outside the prefix through a link: "
                            f"{', '.join(refused)}")

    def status(self) -> None:
        known = (self.manifest or {}).get("files", {})
        files, _, _ = local_tree(self.prefix) if self.prefix.is_dir() else ({}, set(), {})

        def on_console(key: str, path: Path) -> bool:
            old = known.get(key)
            if old is None or (key not in REGISTRY and old[0] != path.stat().st_size):
                return False
            return old == file_entry(key, path)
        changed = [key for key, path in files.items() if not on_console(key, path)]
        log(f"{self.slug}: {len(files)} local files, {len(changed)} not on the console yet; "
            f"console registry changed: {', '.join(self.registry_drift()) or 'no'}")


def interrupted(signum, frame) -> None:
    raise Interrupted(f"signal {signum}")


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
    parser.add_argument("--cpu-dll", help="wowprospero.dll to put in the prefix's system32 (push)")
    parser.add_argument("--trust-size", action="store_true",
                        help="take files the console already has at the same size as sent, "
                             "without comparing their bytes (push)")
    parser.add_argument("--transport", choices=("ftp", "ps5upload"), default="ftp",
                        help="send the game's files over FTP or through ps5upload's payload (push)")
    parser.add_argument("--ps5upload-client",
                        help="ps5upload's ps5upload-lab client (default: $PS5UPLOAD_LAB, then PATH)")
    args = parser.parse_args(argv)
    if not args.host and (remote is None or args.transport == "ps5upload"):
        print("pw_prefix: give --host or PS5_HOST", file=sys.stderr)
        return 2
    previous = signal.signal(signal.SIGTERM, interrupted)
    try:
        sync = Sync(args, remote)
        getattr(sync, args.command)()
    except (KeyboardInterrupt, Interrupted):
        print("pw_prefix: interrupted; what was sent is recorded, push again to resume", file=sys.stderr)
        return 130
    except (SyncError, OSError, *ftplib.all_errors) as error:
        print(f"pw_prefix: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, previous)
    return 0


if __name__ == "__main__":
    sys.exit(main())
