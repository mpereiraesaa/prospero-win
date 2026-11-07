#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare an installed Windows game for Prospero Win without running Wine on Windows.

On Linux, ``template`` creates a clean, portable WoW64 prefix ZIP using the
pinned host Wine. Copy that ZIP to Windows. There, ``import`` copies an already
installed game into the prefix, optionally installs a pinned DXVK archive,
and writes the profile used by tools/pw_prefix.py to push the game.
"""
from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path, PureWindowsPath

from pw_prefix import CPU_DLL, LINK_TABLE, local_tree

TEMPLATE_FORMAT = "prospero-win-prefix-v1"
EXPECTED_WINE = "wine-11.17-54-g490f6d5dcb"
DXVK = {
    "2.6.2": "17761876556afd55736cb895d184f5a1c55d43350f1b1e3b129f8d28706d7992",
    "2.6.2-prospero1": "72a4d7e279f522ad9bac420caf13665bcdd28b297337e1364c31c5bda24ef3f2",
    "2.6.2-prospero2": "c00c1cd7df7ce70e89130b61c1fcd2d3a18ac473f274a219da4478147e4f3c90",
}
DXVK_DLLS = ("d3d8", "d3d9", "d3d10core", "d3d11", "dxgi")
SLUG = re.compile(r"[a-z0-9][a-z0-9-]{0,63}\Z")
INVALID_COMPONENT = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)),
            *(f"lpt{i}" for i in range(1, 10))}


class ImportError_(Exception):
    pass


def require_component(name: str) -> None:
    if (not name or name in (".", "..") or name.endswith((" ", ".")) or
            INVALID_COMPONENT.search(name) or name.split(".", 1)[0].casefold() in RESERVED):
        raise ImportError_(f"not a portable Windows path component: {name!r}")


def target_parts(value: str) -> tuple[str, ...]:
    path = PureWindowsPath(value)
    if path.drive.upper() != "C:" or path.root != "\\":
        raise ImportError_(f"install path must be an absolute C: path: {value!r}")
    parts = tuple(path.parts[1:])
    if not parts:
        raise ImportError_("install path cannot be the C: drive root")
    for part in parts:
        require_component(part)
    return parts


def relative_parts(value: str) -> tuple[str, ...]:
    path = PureWindowsPath(value)
    if path.drive or path.root or not path.parts:
        raise ImportError_(f"executable must be relative to the game folder: {value!r}")
    for part in path.parts:
        require_component(part)
    return tuple(path.parts)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pe_architecture(path: Path) -> str:
    with path.open("rb") as source:
        header = source.read(4096)
    if header[:2] != b"MZ" or len(header) < 0x40:
        raise ImportError_(f"{path}: not a Windows executable")
    offset = struct.unpack_from("<I", header, 0x3c)[0]
    if offset + 26 > len(header) or header[offset:offset + 4] != b"PE\0\0":
        raise ImportError_(f"{path}: not a PE executable")
    magic = struct.unpack_from("<H", header, offset + 24)[0]
    if magic not in (0x10b, 0x20b):
        raise ImportError_(f"{path}: unsupported PE architecture")
    return "pe32" if magic == 0x10b else "pe64"


def make_template(args: argparse.Namespace) -> None:
    wine = Path(args.wine).expanduser().resolve()
    wineserver = wine.with_name("wineserver")
    cpu_dll = Path(args.cpu_dll).expanduser().resolve()
    output = Path(args.output).expanduser().resolve()
    if not wine.is_file() or not wineserver.is_file() or not cpu_dll.is_file():
        raise ImportError_("template needs a host Wine with wineserver beside it and a wowprospero.dll")
    if output.exists():
        raise ImportError_(f"template already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pw-template-") as temp:
        prefix = Path(temp) / "prefix"
        env = dict(os.environ, WINEPREFIX=str(prefix), WINEARCH="win64", USER="prospero",
                   LOGNAME="prospero", WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d;mshtml=;mscoree=")
        subprocess.run([str(wine), "wineboot", "--init"], env=env, check=True)
        subprocess.run([str(wineserver), "-w"], env=env, check=True)
        cpu_target = prefix / CPU_DLL
        cpu_target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(cpu_dll, cpu_target)
        files, dirs, links = local_tree(prefix)
        if links.get("dosdevices", {}).get("c:") != "../drive_c" or \
                links.get("dosdevices", {}).get("z:") != "/":
            raise ImportError_("Wine did not create the expected C: and Z: drive links")
        wine_version = subprocess.check_output([str(wine), "--version"], text=True).strip()
        if wine_version != EXPECTED_WINE:
            raise ImportError_(f"template needs {EXPECTED_WINE}, got {wine_version}")
        metadata = {"format": TEMPLATE_FORMAT, "wine": wine_version, "user": "prospero",
            "cpu_dll_sha256": digest(cpu_target)}
        partial = output.with_name(output.name + ".partial")
        try:
            with zipfile.ZipFile(partial, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6,
                                 allowZip64=True) as archive:
                archive.writestr("template.json", json.dumps(metadata, sort_keys=True) + "\n")
                for directory in sorted(dirs):
                    if directory:
                        archive.writestr(f"prefix/{directory}/", b"")
                for key, path in sorted(files.items()):
                    archive.write(path, f"prefix/{key}")
                for directory, table in sorted(links.items()):
                    text = "".join(f"{name}\t{target}\n" for name, target in sorted(table.items()))
                    archive.writestr(f"prefix/{directory}/{LINK_TABLE}", text.encode("utf-8"))
            os.replace(partial, output)
        finally:
            partial.unlink(missing_ok=True)
    print(f"template ready: {output} ({output.stat().st_size / (1 << 20):.1f} MiB)")


def extract_template(archive_path: Path, stage: Path) -> dict:
    with zipfile.ZipFile(archive_path) as archive:
        try:
            metadata = json.loads(archive.read("template.json"))
        except (KeyError, ValueError) as error:
            raise ImportError_(f"invalid prefix template metadata: {error}") from error
        if (metadata.get("format") != TEMPLATE_FORMAT or metadata.get("user") != "prospero" or
                metadata.get("wine") != EXPECTED_WINE):
            raise ImportError_("unsupported prefix template format, Windows user, or Wine version")
        seen = set()
        for item in archive.infolist():
            name = item.filename
            if name == "template.json":
                continue
            if not name.startswith("prefix/"):
                raise ImportError_(f"unexpected template entry: {name}")
            parts = name.rstrip("/").split("/")[1:]
            if not parts:
                continue
            for part in parts:
                require_component(part)
            key = "/".join(parts).casefold()
            if key in seen:
                raise ImportError_(f"duplicate case-insensitive template path: {name}")
            seen.add(key)
            if (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ImportError_(f"template contains a symbolic link: {name}")
            target = stage.joinpath(*parts)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item) as source, target.open("wb") as out:
                    shutil.copyfileobj(source, out, 4 << 20)
    for name in ("system.reg", "user.reg", "userdef.reg", CPU_DLL,
                 "dosdevices/.pw-symlinks"):
        if not (stage / name).is_file():
            raise ImportError_(f"template lacks {name}")
    links = (stage / "dosdevices/.pw-symlinks").read_text(encoding="utf-8")
    if "c:\t../drive_c\n" not in links or "z:\t/\n" not in links:
        raise ImportError_("template lacks the C: or Z: Wine drive mapping")
    if digest(stage / CPU_DLL) != metadata.get("cpu_dll_sha256"):
        raise ImportError_("template wowprospero.dll checksum mismatch")
    return metadata


def wine_string(value: str) -> str:
    if "\n" in value or "\r" in value or "\0" in value:
        raise ImportError_("registry values cannot contain newlines or NUL")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def set_wine_registry(prefix: Path, setting: str) -> None:
    try:
        key, name, value = setting.split("|", 2)
    except ValueError as error:
        raise ImportError_("--reg-string needs HIVE\\Key|Value name|Value") from error
    hive, separator, subkey = key.partition("\\")
    hive = hive.upper()
    if not separator or not subkey or hive not in ("HKLM", "HKEY_LOCAL_MACHINE", "HKCU", "HKEY_CURRENT_USER"):
        raise ImportError_(f"unsupported registry key: {key}")
    for part in subkey.split("\\"):
        if not part or part in (".", ".."):
            raise ImportError_(f"invalid registry key: {key}")
    path = prefix / ("system.reg" if hive in ("HKLM", "HKEY_LOCAL_MACHINE") else "user.reg")
    section = "[" + subkey.replace("\\", "\\\\") + "]"
    line = f"{wine_string(name)}={wine_string(value)}\n"
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    lines = text.splitlines(keepends=True)
    headings = [i for i, current in enumerate(lines) if current.startswith("[") and "]" in current]
    match = next((i for i in headings if lines[i].split("]", 1)[0].casefold() + "]" == section.casefold()), None)
    if match is None:
        insert = next((i for i in headings if lines[i].split("]", 1)[0].casefold() + "]" > section.casefold()), len(lines))
        filetime = int(time.time() * 10_000_000) + 116_444_736_000_000_000
        lines[insert:insert] = [f"{section} {int(time.time())}\n", f"#time={filetime:x}\n", line, "\n"]
    else:
        end = next((i for i in headings if i > match), len(lines))
        prefix_name = wine_string(name).casefold() + "="
        existing = next((i for i in range(match + 1, end) if lines[i].casefold().startswith(prefix_name)), None)
        if existing is not None:
            lines[existing] = line
        else:
            insert = end - 1 if end > match and not lines[end - 1].strip() else end
            lines.insert(insert, line)
    path.write_text("".join(lines), encoding="utf-8", errors="surrogateescape")


def install_dxvk(prefix: Path, archive: Path, version: str) -> None:
    if version not in DXVK:
        raise ImportError_(f"unsupported pinned DXVK version: {version}")
    if digest(archive) != DXVK[version]:
        raise ImportError_(f"DXVK archive does not match pinned SHA-256 for {version}")
    with tarfile.open(archive, "r:gz") as release:
        for bits, folder in (("x64", "system32"), ("x32", "syswow64")):
            for dll in DXVK_DLLS:
                member = release.extractfile(f"dxvk-{version}/{bits}/{dll}.dll")
                if member is None:
                    raise ImportError_(f"DXVK archive lacks {bits}/{dll}.dll")
                target = prefix / "drive_c/windows" / folder / f"{dll}.dll"
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(member.read())
    for dll in DXVK_DLLS:
        set_wine_registry(prefix, f"HKCU\\Software\\Wine\\DllOverrides|{dll}|native")


def import_game(args: argparse.Namespace) -> None:
    if not SLUG.fullmatch(args.slug):
        raise ImportError_(f"invalid profile id: {args.slug!r}")
    if any(c in args.name for c in "\r\n\0") or not args.name.strip():
        raise ImportError_("game name must be one nonempty line")
    install = target_parts(args.install_path)
    exe = relative_parts(args.exe)
    source = Path(args.game_dir).expanduser().resolve()
    if not source.is_dir() or not source.joinpath(*exe).is_file():
        raise ImportError_("game directory or selected executable is missing")
    for path in source.rglob("*"):
        if path.is_symlink():
            raise ImportError_(f"game folder contains a symbolic link: {path}")
    library = Path(args.library).expanduser().resolve()
    prefixes = library / "prefixes"
    profile = library / "profiles" / f"{args.slug}.profile"
    final = prefixes / args.slug
    if final.exists() or profile.exists():
        raise ImportError_(f"{args.slug} already exists in {library}; choose another slug")
    if source == prefixes or prefixes in source.parents or source in prefixes.parents:
        raise ImportError_("game source must be outside the destination library's prefixes folder")
    if args.graphics == "dxvk" and not args.dxvk_archive:
        raise ImportError_("DXVK graphics requires --dxvk-archive")
    if args.graphics != "dxvk" and args.dxvk_archive:
        raise ImportError_("--dxvk-archive requires --graphics dxvk")
    if not re.fullmatch(r"[0-9]{3,4}x[0-9]{3,4}", args.desktop):
        raise ImportError_("--desktop must be WIDTHxHEIGHT")
    width, height = (int(x) for x in args.desktop.split("x"))
    if not (320 <= width <= 3840 and 200 <= height <= 2160):
        raise ImportError_("desktop size is outside Prospero Win's supported range")
    preset = None
    input_mode = args.input_mode or "xinput"
    if args.input_preset:
        preset = Path(args.input_preset).expanduser().resolve()
        if not preset.is_file() or preset.suffix.lower() != ".input" or not SLUG.fullmatch(preset.stem):
            raise ImportError_("--input-preset needs an existing .input file with a valid name")
        config = configparser.RawConfigParser()
        try:
            config.read(preset, encoding="utf-8")
        except (configparser.Error, UnicodeError) as error:
            raise ImportError_(f"invalid input preset {preset}: {error}") from error
        preset_mode = config.get("input", "mode", fallback="")
        if preset_mode and preset_mode not in ("keyboard", "xinput"):
            raise ImportError_(f"invalid input mode in {preset}: {preset_mode}")
        if args.input_mode and preset_mode and args.input_mode != preset_mode:
            raise ImportError_("--input-mode conflicts with the selected input preset")
        input_mode = args.input_mode or preset_mode or "xinput"
        existing = library / "input" / preset.name
        if existing.is_file() and digest(existing) != digest(preset):
            raise ImportError_(f"input preset {existing} already exists with different contents")
    prefixes.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{args.slug}-", dir=prefixes))
    try:
        metadata = extract_template(Path(args.template).expanduser().resolve(), stage)
        target = stage / "drive_c" / Path(*install)
        if target.exists():
            raise ImportError_(f"template already contains C:\\{'\\'.join(install)}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target)
        game_exe = target.joinpath(*exe)
        architecture = pe_architecture(game_exe)
        dll_overrides = ""
        if args.graphics == "dxvk":
            install_dxvk(stage, Path(args.dxvk_archive).expanduser().resolve(), args.dxvk_version)
            dll_overrides = "dll_overrides = " + ",".join(DXVK_DLLS) + "=n\n"
        for setting in args.reg_string:
            set_wine_registry(stage, setting)
        windows_exe = str(PureWindowsPath(args.install_path, *exe))
        windows_working_dir = str(PureWindowsPath(args.install_path, *exe[:-1]))
        lines = [f"; {args.name}: imported from an installed Windows game.", "[application]",
                 f"id = {args.slug}", f"name = {args.name}",
                 f"executable = {windows_exe}", f"working_directory = {windows_working_dir}"]
        if args.arguments:
            if any(c in args.arguments for c in "\r\n\0"):
                raise ImportError_("arguments must be one line")
            lines.append(f"arguments = {args.arguments}")
        if dll_overrides:
            lines.append(dll_overrides.rstrip("\n"))
        lines += [f"prefix = {args.slug}", "runtime = wine-wow64",
                  f"architecture = {architecture}", f"graphics = {args.graphics}", "",
                  "[display]", f"desktop = {args.desktop}", "scaling = fit", "",
                  "[input]", f"mode = {input_mode}"]
        if preset:
            lines.append(f"preset = {preset.stem}")
        profile_text = "\n".join(lines) + "\n"
        os.replace(stage, final)
        profile.parent.mkdir(parents=True, exist_ok=True)
        profile.write_text(profile_text, encoding="utf-8")
        if preset:
            input_dir = library / "input"
            input_dir.mkdir(parents=True, exist_ok=True)
            if preset != input_dir / preset.name:
                shutil.copy2(preset, input_dir / preset.name)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print(f"imported {args.slug}: {final}, {profile} (template {metadata['wine']})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    template = commands.add_parser("template", help="Linux: make a clean Wine prefix template ZIP")
    template.add_argument("--wine", required=True)
    template.add_argument("--cpu-dll", required=True, help="console wowprospero.dll")
    template.add_argument("--output", required=True)
    game = commands.add_parser("import", help="Windows: import an already installed game")
    game.add_argument("--template", required=True)
    game.add_argument("--game-dir", required=True)
    game.add_argument("--exe", required=True, help="executable relative to --game-dir")
    game.add_argument("--install-path", required=True, help=r"path in the prefix, e.g. C:\Games\Game")
    game.add_argument("--library", required=True)
    game.add_argument("--slug", required=True)
    game.add_argument("--name", required=True)
    game.add_argument("--graphics", choices=("gdi", "dxvk", "opengl", "auto"), default="dxvk")
    game.add_argument("--dxvk-archive", help="pinned DXVK release tar.gz (required for dxvk graphics)")
    game.add_argument("--dxvk-version", choices=tuple(DXVK), default="2.6.2")
    game.add_argument("--input-mode", choices=("xinput", "keyboard"),
                      help="defaults to the preset's mode, or xinput when no preset is given")
    game.add_argument("--input-preset", help="optional .input controller preset to copy into the library")
    game.add_argument("--desktop", default="1920x1080")
    game.add_argument("--arguments", default="")
    game.add_argument("--reg-string", action="append", default=[], metavar="KEY|NAME|VALUE",
                      help="set a Wine registry string, e.g. HKLM\\Software\\Game|InstallPath|C:\\Games\\Game")
    args = parser.parse_args(argv)
    try:
        if args.command == "template":
            make_template(args)
        else:
            import_game(args)
    except (ImportError_, OSError, ValueError, zipfile.BadZipFile, tarfile.TarError,
            subprocess.CalledProcessError) as error:
        print(f"pw_import_windows: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
