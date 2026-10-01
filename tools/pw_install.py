#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Install a Windows game for Prospero Win from a Lutris installer script.

Games are installed on the PC, not on the console: installers start
processes, show wizards and ask for keys, and the host has the same Wine as
the title (the pinned WoW64 build, wine-11.17-54-g490f6d5), so the prefix it
writes is one the console reads as is. The recipe is a Lutris installer
script (https://github.com/lutris/lutris/blob/master/docs/installers.rst),
the format the community already maintains for thousands of games; this
runner executes the directives and Wine tasks those games use, with the
pinned Wine instead of Lutris's own builds, and refuses any it does not
implement rather than skipping them.

Each game gets its own prefix, LIBRARY/prefixes/<slug>, which is $GAMEDIR,
and a profile, LIBRARY/profiles/<slug>.profile, generated from the script's
`game` section and a `prospero` block (ignored by Lutris) for what only the
console needs: the desktop size, scaling and input. LIBRARY mirrors
/data/prospero-win on the console.

DXVK follows Lutris's own keys (`wine: {dxvk: true, dxvk_version: 2.6.2}`):
the release's DLLs go into the prefix's system32 and syswow64 and are set
native, in the prefix's registry for the host and in the profile's
dll_overrides for the title.

Usage:
    pw_install.py SCRIPT.yml --library DIR --wine PATH [--file ID=PATH ...]
        [--input ID=VALUE ...] [--disc DIR] [--resolution WxH] [--keep-cache]
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path, PureWindowsPath

try:
    import yaml
except ImportError:  # pragma: no cover - reported to the user
    yaml = None

# Pinned downloads: the version Lutris would fetch, checked by hash.
DXVK_RELEASES = {
    "2.6.2": ("https://github.com/doitsujin/dxvk/releases/download/v2.6.2/dxvk-2.6.2.tar.gz",
              "17761876556afd55736cb895d184f5a1c55d43350f1b1e3b129f8d28706d7992"),
}
DXVK_DLLS = ("d3d8", "d3d9", "d3d10core", "d3d11", "dxgi")
WINETRICKS = ("20260125", "https://raw.githubusercontent.com/Winetricks/winetricks/20260125/src/winetricks",
              "431f82fc74000e6c864409f1d8fb495d696c03928808e3e8acffc45179312a7b")
# Wine Gecko, which Wine's mshtml needs (an installer's license shown in an
# Internet Explorer control, for one): the version and hashes the pinned Wine
# names in dlls/appwiz.cpl/addons.c, both architectures for WoW64.
GECKO = ("2.47.4", {"x86": "26cecc47706b091908f7f814bddb074c61beb8063318e9efc5a7f789857793d6",
                    "x86_64": "e590b7d988a32d6aa4cf1d8aa3aa3d33766fdd4cf4c89c2dcc2095ecb28d066f"})
WINE_CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "wine"
DOWNLOADS = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "prospero-win"
SLUG = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
REG_TYPES = {"REG_SZ", "REG_DWORD", "REG_BINARY"}
PROSPERO_DISPLAY = ("desktop", "scaling", "view", "show_fps", "refresh", "opengl_thread")
PROSPERO_INPUT = ("preset", "mode", "mouse", "mouse_speed")


class InstallError(Exception):
    pass


def log(message: str) -> None:
    print(f"pw_install: {message}", flush=True)


def fetch(url: str, sha256: str, name: str, directory: Path | None = None) -> Path:
    """A pinned download, kept in the user's cache and checked each time."""
    directory = directory or DOWNLOADS
    path = directory / name
    if not path.is_file():
        directory.mkdir(parents=True, exist_ok=True)
        log(f"downloading {url}")
        with urllib.request.urlopen(url) as response, open(f"{path}.part", "wb") as out:
            shutil.copyfileobj(response, out)
        os.replace(f"{path}.part", path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        raise InstallError(f"{path} does not match its pinned SHA-256 {sha256}")
    return path


def pe_architecture(path: Path) -> str:
    """pe32 or pe64, from the PE optional header's magic."""
    data = path.read_bytes()
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise InstallError(f"{path} is not a PE executable")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[offset:offset + 4] != b"PE\0\0":
        raise InstallError(f"{path} is not a PE executable")
    magic = struct.unpack_from("<H", data, offset + 24)[0]
    return {0x10B: "pe32", 0x20B: "pe64"}.get(magic) or _refuse(f"{path}: unknown PE magic {magic:#x}")


def _refuse(message: str):
    raise InstallError(message)


class Installer:
    def __init__(self, document: dict, script_path: Path, args: argparse.Namespace):
        self.document = document
        self.script = document.get("script", document)
        if document.get("runner", self.script.get("runner", "wine")) != "wine":
            raise InstallError("only wine scripts can run on Prospero Win")
        self.slug = args.slug or document.get("game_slug") or document.get("slug")
        if not self.slug or not SLUG.match(self.slug):
            raise InstallError(f"the script's slug {self.slug!r} is not a profile id (use --slug)")
        self.library = Path(args.library).resolve()
        self.gamedir = self.library / "prefixes" / self.slug
        self.cache = self.library / ".cache" / self.slug
        # As given, not resolved: a build tree's wine is a symlink into
        # tools/wine, and its wineserver is server/wineserver beside it.
        self.wine = Path(os.path.abspath(args.wine))
        candidates = [self.wine.with_name("wineserver"), self.wine.parent / "server" / "wineserver"]
        self.wineserver = next((path for path in candidates if path.is_file()), None)
        if not self.wine.is_file() or self.wineserver is None:
            raise InstallError(f"{args.wine} is not a Wine with a wineserver beside it")
        width, height = (args.resolution or "1920x1080").split("x")
        self.variables = {
            "GAMEDIR": str(self.gamedir), "CACHE": str(self.cache), "HOME": str(Path.home()),
            "SCRIPTDIR": str(script_path.parent.resolve()), "WINEBIN": str(self.wine),
            "RESOLUTION": f"{width}x{height}", "RESOLUTION_WIDTH": width, "RESOLUTION_HEIGHT": height,
        }
        for key, value in (self.script.get("variables") or {}).items():
            self.variables[str(key)] = str(value)
        self.given_files = dict(item.split("=", 1) for item in args.file)
        self.given_inputs = dict(item.split("=", 1) for item in args.input)
        self.disc = args.disc
        self.files: dict[str, Path] = {}
        wine_section = self.script.get("wine") or {}
        self.overrides = {str(k).removesuffix(".dll"): str(v) for k, v in (wine_section.get("overrides") or {}).items()}
        self.env = {str(k): self.expand(str(v)) for k, v in ((self.script.get("system") or {}).get("env") or {}).items()}
        self.dxvk = wine_section.get("dxvk", False)
        self.dxvk_version = str(wine_section.get("dxvk_version", "2.6.2"))

    # --- variables and paths --------------------------------------------
    def expand(self, value):
        if isinstance(value, str):
            for name in sorted(self.variables, key=len, reverse=True):
                value = value.replace(f"${name}", self.variables[name])
            return value
        if isinstance(value, list):
            return [self.expand(item) for item in value]
        if isinstance(value, dict):
            return {key: self.expand(item) for key, item in value.items()}
        return value

    def path(self, reference: str) -> Path:
        """A file id from the files section, or a path after expansion."""
        if reference in self.files:
            return self.files[reference]
        return Path(self.expand(reference))

    # --- files -------------------------------------------------------------
    def resolve_files(self) -> None:
        for item in self.script.get("files") or []:
            (file_id, spec), = item.items()
            filename = digest = None
            if isinstance(spec, dict):
                # sha256 is ours, not Lutris's: a download pinned to its hash,
                # as a third-party mod or DLL should be (Lutris ignores it).
                filename, digest, spec = spec.get("filename"), spec.get("sha256"), spec.get("url", "")
            spec = self.expand(str(spec))
            if file_id in self.given_files:
                path = Path(self.given_files[file_id]).expanduser().resolve()
            elif spec.startswith("N/A"):
                raise InstallError(f"file {file_id!r}: {spec[4:] or 'supply it'} (--file {file_id}=PATH)")
            elif digest is not None:
                if not re.match(r"^(https?|file)://", spec):
                    raise InstallError(f"file {file_id!r}: sha256 is for downloads, not {spec!r}")
                path = fetch(spec, str(digest).lower(), filename or spec.rsplit("/", 1)[-1])
            elif re.match(r"^https?://", spec):
                path = self.cache / (filename or spec.rsplit("/", 1)[-1])
                self.cache.mkdir(parents=True, exist_ok=True)
                log(f"downloading {file_id}: {spec}")
                with urllib.request.urlopen(spec) as response, open(path, "wb") as out:
                    shutil.copyfileobj(response, out)
            elif spec.startswith("$STEAM"):
                raise InstallError(f"file {file_id!r}: Steam sources are not supported")
            else:
                path = Path(spec).expanduser().resolve()
            if not path.exists():
                raise InstallError(f"file {file_id!r}: {path} does not exist")
            self.files[file_id] = path
            log(f"file {file_id} = {path}")

    # --- Wine --------------------------------------------------------------
    def wine_env(self, task: dict | None = None) -> dict:
        env = dict(os.environ, **self.env)
        env.update({str(k): self.expand(str(v)) for k, v in ((task or {}).get("env") or {}).items()})
        prefix = self.expand((task or {}).get("prefix") or self.script.get("game", {}).get("prefix") or "$GAMEDIR")
        # As Lutris does, Wine adds no menu entries or file associations to
        # the PC's desktop.
        overrides = {"winemenubuilder.exe": "d", **self.overrides}
        overrides.update({str(k).removesuffix(".dll"): str(v) for k, v in ((task or {}).get("overrides") or {}).items()})
        # The title runs Wine as USER=prospero (native/wine64_main.c): the
        # prefix's profile folder and registry paths must be that user's.
        env.update(USER="prospero", LOGNAME="prospero")
        env.update(WINEPREFIX=str(Path(prefix).resolve()), WINEARCH="win64",
                   WINEDEBUG=env.get("WINEDEBUG", "-all"),
                   WINEDLLOVERRIDES=";".join(f"{name}={mode}" for name, mode in overrides.items()))
        if (task or {}).get("arch") == "win32":
            log("arch win32: the pinned Wine is WoW64, so the prefix is win64 and runs 32-bit programs")
        return env

    def run(self, command: list, env: dict, cwd: Path | None = None, codes: tuple = (0,)) -> None:
        log("run " + " ".join(str(part) for part in command))
        result = subprocess.run([str(part) for part in command], env=env, cwd=cwd)
        # An installer's helpers outlive it: wait until the prefix is idle.
        subprocess.run([str(self.wineserver), "-w"], env=env)
        if result.returncode not in codes:
            raise InstallError(f"{command[0]} exited with {result.returncode} (accepted: {codes})")

    def regedit(self, lines: list[str], task: dict | None) -> None:
        with tempfile.NamedTemporaryFile("w", suffix=".reg", delete=False, encoding="utf-8") as reg:
            reg.write("REGEDIT4\n\n" + "\n".join(lines) + "\n")
        try:
            self.run([self.wine, "regedit", "/S", reg.name], self.wine_env(task))
        finally:
            os.unlink(reg.name)

    @staticmethod
    def reg_value(value, kind: str) -> str:
        if kind == "REG_SZ":
            return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'
        if kind == "REG_DWORD":
            number = int(str(value), 16) if re.fullmatch(r"[0-9a-fA-F]{8}", str(value)) else int(value)
            return f"dword:{number:08x}"
        return "hex:" + ",".join(f"{byte:02x}" for byte in bytes.fromhex(str(value).replace(",", "")))

    def task(self, task: dict) -> None:
        name = task.get("name")
        env = self.wine_env(task)
        if name == "create_prefix":
            # As in Lutris, Gecko and Mono only when the script asks for them.
            # wineboot installs Gecko from Wine's download cache, where the
            # pinned packages are put first.
            if task.get("install_gecko"):
                version, digests = GECKO
                for arch, digest in digests.items():
                    fetch(f"https://dl.winehq.org/wine/wine-gecko/{version}/wine-gecko-{version}-{arch}.msi",
                          digest, f"wine-gecko-{version}-{arch}.msi", WINE_CACHE)
            else:
                env["WINEDLLOVERRIDES"] += ";mshtml="
            if not task.get("install_mono"):
                env["WINEDLLOVERRIDES"] += ";mscoree="
            self.run([self.wine, "wineboot", "--init"], env)
        elif name == "wineexec":
            executable = self.path(str(task["executable"]))
            if not executable.is_absolute():
                executable = self.gamedir / executable
            codes = tuple(int(code) for code in str(task.get("return_code", 0)).split(","))
            args = self.expand(str(task.get("args", "")))
            cwd = Path(self.expand(task["working_dir"])) if task.get("working_dir") else executable.parent
            if task.get("description"):
                log(task["description"])
            self.run([self.wine, executable, *_split(args)], env, cwd, codes)
        elif name == "winetricks":
            version, url, digest = WINETRICKS
            script = fetch(url, digest, f"winetricks-{version}")
            env["WINE"] = str(self.wine)
            env["WINESERVER"] = str(self.wineserver)
            flags = ["-q"] if task.get("silent", True) else []
            self.run(["sh", script, *flags, *self.expand(str(task["app"])).split()], env)
        elif name == "set_regedit":
            kind = str(task.get("type", "REG_SZ"))
            if kind not in REG_TYPES:
                raise InstallError(f"set_regedit type {kind} is not supported")
            key = "@" if task["key"] in ("", "@") else f'"{task["key"]}"'
            self.regedit([f"[{self.expand(task['path'])}]", f"{key}={self.reg_value(self.expand(task['value']), kind)}"], task)
        elif name == "delete_registry_key":
            path = self.expand(task["path"])
            lines = [f"[{path}]", f'"{task["key"]}"=-'] if task.get("key") else [f"[-{path}]"]
            self.regedit(lines, task)
        elif name == "set_regedit_file":
            self.run([self.wine, "regedit", "/S", self.path(str(task["filename"]))], env)
        elif name == "winekill":
            subprocess.run([str(self.wineserver), "-k"], env=env)
        elif name == "eject_disc":
            pass
        else:
            raise InstallError(f"Wine task {name!r} is not supported")

    # --- directives ----------------------------------------------------------
    def directive(self, step: dict) -> None:
        (kind, body), = step.items()
        if kind == "task":
            self.task(body)
        elif kind in ("move", "copy", "merge"):
            source, target = self.path(str(body["src"])), Path(self.expand(body["dst"]))
            # Lutris: a directory destination receives the source by name.
            if kind == "move":
                target.parent.mkdir(parents=True, exist_ok=True)
            else:
                target.mkdir(parents=True, exist_ok=True)
            if kind == "move":
                destination = target / source.name if target.is_dir() else target
                shutil.move(source, destination)
                self.files.update({k: destination for k, v in self.files.items() if v == source})
            elif source.is_dir():
                shutil.copytree(source, target, dirs_exist_ok=True)
            else:
                shutil.copy2(source, target / source.name if target.is_dir() else target)
            if kind == "merge" and body.get("remove_source"):
                shutil.rmtree(source) if source.is_dir() else source.unlink()
        elif kind == "extract":
            self.extract(self.path(str(body.get("file", body.get("src")))), Path(self.expand(body.get("dst", "$CACHE"))),
                         body.get("format"))
        elif kind == "chmodx":
            path = self.path(str(body))
            path.chmod(path.stat().st_mode | 0o111)
        elif kind == "execute":
            env = dict(os.environ, **self.env, **{str(k): self.expand(str(v)) for k, v in (body.get("env") or {}).items()})
            cwd = self.expand(body["working_dir"]) if body.get("working_dir") else None
            if "command" in body:
                command = ["sh", "-c", self.expand(body["command"])]
            else:
                command = [self.path(str(body["file"])), *_split(self.expand(str(body.get("args", ""))))]
            log(f"execute {command}")
            subprocess.run([str(part) for part in command], env=env, cwd=cwd, check=True)
        elif kind == "write_file":
            path = Path(self.expand(body["file"]))
            path.parent.mkdir(parents=True, exist_ok=True)
            with open(path, body.get("mode", "w"), encoding="utf-8") as out:
                out.write(self.expand(str(body["content"])))
        elif kind == "write_config":
            self.write_config(body)
        elif kind == "write_json":
            path = Path(self.expand(body["file"]))
            data = json.loads(path.read_text()) if path.is_file() and body.get("merge", True) else {}
            _deep_merge(data, self.expand(body["data"]))
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(data, indent=2))
        elif kind == "input_menu":
            self.input_menu(body)
        elif kind == "insert-disc":
            if not self.disc or not (Path(self.disc) / body["requires"]).exists():
                raise InstallError(f"insert-disc: give --disc DIR containing {body['requires']}")
            self.variables["DISC"] = str(Path(self.disc).resolve())
        else:
            raise InstallError(f"directive {kind!r} is not supported")

    def extract(self, archive: Path, target: Path, kind: str | None) -> None:
        target.mkdir(parents=True, exist_ok=True)
        name = archive.name.lower()
        if kind == "zip" or (not kind and name.endswith(".zip")):
            with zipfile.ZipFile(archive) as z:
                z.extractall(target)
        elif (kind or "").startswith("t") or re.search(r"\.(tar|tgz|tar\.gz|tar\.xz|txz|tar\.bz2)$", name):
            with tarfile.open(archive) as t:
                t.extractall(target, filter="data")
        elif shutil.which("7z"):
            subprocess.run(["7z", "x", "-y", f"-o{target}", str(archive)], check=True, stdout=subprocess.DEVNULL)
        else:
            raise InstallError(f"extract {archive}: format {kind or name} needs 7z on the PATH")

    def write_config(self, body: dict) -> None:
        path = Path(self.expand(body["file"]))
        config = configparser.RawConfigParser(strict=False)
        config.optionxform = str
        if path.is_file() and body.get("merge", True):
            config.read(path, encoding="utf-8")
        data = self.expand(body.get("data") or {body["section"]: {body["key"]: body["value"]}})
        for section, values in data.items():
            if not config.has_section(section):
                config.add_section(section)
            for key, value in values.items():
                config.set(section, key, str(value))
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as out:
            config.write(out, space_around_delimiters=False)

    def input_menu(self, body: dict) -> None:
        options = [next(iter(option.items())) for option in body["options"]]
        choice = self.given_inputs.get(body["id"])
        if choice is None and sys.stdin.isatty():
            for value, label in options:
                print(f"  {value}: {label}")
            choice = input(f"{body.get('description', body['id'])} [{body.get('preselect', '')}] ").strip()
        choice = choice or body.get("preselect")
        if choice not in {value for value, _ in options}:
            raise InstallError(f"input {body['id']}: choose one of {[v for v, _ in options]} (--input {body['id']}=VALUE)")
        self.variables[f"INPUT_{body['id']}"] = self.variables["INPUT"] = str(choice)

    # --- DXVK and the profile ------------------------------------------------
    def graphics_mode(self) -> str:
        prospero = self.document.get("prospero") or self.script.get("prospero") or {}
        mode = prospero.get("graphics", "dxvk" if self.dxvk else "gdi")
        if mode not in ("auto", "gdi", "dxvk", "opengl"):
            raise InstallError(f"prospero.graphics: unsupported backend {mode!r}")
        return mode

    def install_dxvk(self) -> list[str]:
        mode = self.graphics_mode()
        if mode != "dxvk" and not (mode == "auto" and self.dxvk):
            return []
        if self.dxvk_version not in DXVK_RELEASES:
            raise InstallError(f"dxvk_version {self.dxvk_version} is not pinned (known: {sorted(DXVK_RELEASES)})")
        url, digest = DXVK_RELEASES[self.dxvk_version]
        archive = fetch(url, digest, f"dxvk-{self.dxvk_version}.tar.gz")
        env = self.wine_env()
        prefix = Path(env["WINEPREFIX"]) / "drive_c" / "windows"
        with tarfile.open(archive) as release:
            for bits, folder in (("x64", "system32"), ("x32", "syswow64")):
                for dll in DXVK_DLLS:
                    member = release.extractfile(f"dxvk-{self.dxvk_version}/{bits}/{dll}.dll")
                    (prefix / folder / f"{dll}.dll").write_bytes(member.read())
        self.regedit(["[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]"] +
                     [f'"{dll}"="native"' for dll in DXVK_DLLS], None)
        log(f"DXVK {self.dxvk_version} installed in the prefix")
        return list(DXVK_DLLS)

    def windows_path(self, value: str) -> str:
        """A path under the prefix's drive_c as the game sees it (C:\\...)."""
        path = Path(self.expand(value))
        if not path.is_absolute():
            path = self.gamedir / path
        relative = path.resolve().relative_to((self.gamedir / "drive_c").resolve())
        return str(PureWindowsPath("C:\\", *relative.parts))

    def profile(self, dxvk_dlls: list[str]) -> str:
        game = self.script.get("game") or {}
        if not game.get("exe"):
            raise InstallError("the script's game section names no exe")
        exe = Path(self.expand(game["exe"]))
        exe = exe if exe.is_absolute() else self.gamedir / exe
        if not exe.is_file():
            raise InstallError(f"the game's exe {exe} was not installed")
        prospero = self.document.get("prospero") or self.script.get("prospero") or {}
        name = prospero.get("name") or self.document.get("name") or self.slug
        overrides = {dll: "n" for dll in dxvk_dlls}
        overrides.update(self.overrides)
        if self.graphics_mode() == "opengl":
            overrides["opengl32"] = "b"
        by_mode: dict[str, list[str]] = {}
        for dll, mode in overrides.items():
            by_mode.setdefault(mode, []).append(dll)
        lines = [f"; {name}: generated by tools/pw_install.py from a Lutris installer script.",
                 "[application]", f"id = {self.slug}", f"name = {name}",
                 f"executable = {self.windows_path(str(exe))}",
                 f"working_directory = {self.windows_path(game.get('working_dir') or str(exe.parent))}"]
        if game.get("args"):
            lines.append(f"arguments = {self.expand(str(game['args']))}")
        if by_mode:
            lines.append("dll_overrides = " + ";".join(f"{','.join(dlls)}={mode}" for mode, dlls in by_mode.items()))
        lines += [f"prefix = {self.slug}", "runtime = wine-wow64", f"architecture = {pe_architecture(exe)}",
                  f"graphics = {self.graphics_mode()}"]
        for section, keys in (("display", PROSPERO_DISPLAY), ("input", PROSPERO_INPUT)):
            values = [(key, prospero[section][key]) for key in keys if key in (prospero.get(section) or {})]
            unknown = set(prospero.get(section) or {}) - set(keys)
            if unknown:
                raise InstallError(f"prospero.{section}: unknown keys {sorted(unknown)}")
            if values:
                # YAML's true/false arrive as Python booleans
                lines += ["", f"[{section}]"] + [
                    f"{key} = {str(value).lower() if isinstance(value, bool) else value}" for key, value in values]
        return "\n".join(lines) + "\n"

    # --- the whole install -----------------------------------------------------
    def install(self) -> Path:
        self.graphics_mode()  # Reject an invalid backend before creating a prefix.
        if self.gamedir.exists():
            raise InstallError(f"{self.gamedir} exists: remove it to reinstall {self.slug}")
        self.resolve_files()
        self.gamedir.mkdir(parents=True)
        for step in self.script.get("installer") or []:
            self.directive(step)
        if not (self.gamedir / "system.reg").is_file():
            raise InstallError("no prefix was created: the script needs a create_prefix task")
        dxvk_dlls = self.install_dxvk()
        profile = self.library / "profiles" / f"{self.slug}.profile"
        profile.parent.mkdir(parents=True, exist_ok=True)
        profile.write_text(self.profile(dxvk_dlls))
        subprocess.run([str(self.wineserver), "-w"], env=self.wine_env())
        log(f"installed {self.slug}: prefix {self.gamedir}, profile {profile}")
        if self.document.get("install_complete_text"):
            log(self.document["install_complete_text"])
        return profile


def _split(args: str) -> list[str]:
    return shlex.split(args, posix=True) if args else []


def _deep_merge(base: dict, extra: dict) -> None:
    for key, value in extra.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("script", type=Path)
    parser.add_argument("--library", required=True, help="the local mirror of /data/prospero-win")
    parser.add_argument("--wine", required=True, help="the pinned host Wine (build-wow64/wine)")
    parser.add_argument("--slug", help="the profile id, when the script has no game_slug")
    parser.add_argument("--file", action="append", default=[], metavar="ID=PATH")
    parser.add_argument("--input", action="append", default=[], metavar="ID=VALUE")
    parser.add_argument("--disc", help="a directory holding the disc's files")
    parser.add_argument("--resolution", default="1920x1080")
    parser.add_argument("--keep-cache", action="store_true")
    args = parser.parse_args(argv)
    if yaml is None:
        print("pw_install: PyYAML is required (python3 -m pip install pyyaml)", file=sys.stderr)
        return 2
    installer = None
    try:
        document = yaml.safe_load(args.script.read_text(encoding="utf-8"))
        installer = Installer(document, args.script, args)
        installer.install()
    except (InstallError, KeyError, subprocess.CalledProcessError) as error:
        print(f"pw_install: {error}", file=sys.stderr)
        return 1
    finally:
        if installer and not args.keep_cache:
            shutil.rmtree(installer.cache, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
