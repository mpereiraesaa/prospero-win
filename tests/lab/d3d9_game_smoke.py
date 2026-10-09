#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise supplied production D3D9 DLLs through real Wine builtin D3DX calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ("wine-build", "prefix", "proxy", "service", "backend64", "d3dx", "output"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--fullscreen", action="store_true")
parser.add_argument("--source-effect", action="store_true", help="also require builtin HLSL FX source compilation")
parser.add_argument("--direct-control", action="store_true", help="label a direct PE32 DXVK control run")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
receipt = {"schema": 1, "status": "failed", "console_accessed": False,
           "mode": "direct-control" if args.direct_control else "production-bridge",
           "fullscreen": args.fullscreen, "source_effect": args.source_effect, "commands": [], "inputs": {}}
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def windows(path):
    return "Z:" + str(path).replace("/", chr(92))
def run(command, label, env=None):
    command = [str(x) for x in command]
    with (out / (label + ".log")).open("wb") as log:
        try:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        except subprocess.TimeoutExpired:
            receipt["commands"].append({"command": command, "timeout": True})
            raise
    receipt["commands"].append({"command": command, "exit": result.returncode})
    if result.returncode:
        raise RuntimeError(f"{label} exited {result.returncode}")
    return (out / (label + ".log")).read_text(errors="replace")
try:
    source = Path(__file__).with_suffix(".c")
    receipt["inputs"][str(source)] = digest(source)
    receipt["inputs"][str(source.with_name("d3d9_game_smoke_fx.h"))] = digest(source.with_name("d3d9_game_smoke_fx.h"))
    for name in ("proxy", "service", "backend64", "d3dx"):
        path = getattr(args, name).resolve(strict=True)
        receipt["inputs"][name] = {"path": str(path), "sha256": digest(path)}
    # Freeze supplied bridge binaries beside the client before execution.
    shutil.copyfile(args.proxy, out / "d3d9.dll")
    shutil.copyfile(args.service, out / "service.dll")
    run(["i686-w64-mingw32-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-municode", source, "-o", out / "client.exe"], "compile")
    receipt["client_sha256"] = digest(out / "client.exe")
    # A real BMP file drives D3DXCreateTextureFromFileA, as used by the game mod.
    pixels = bytes([0, 255, 0, 0, 255, 0, 0, 0]) * 2
    bitmap = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    bitmap += struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels
    (out / "texture.bmp").write_bytes(bitmap)
    args.prefix.resolve().mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(WINEPREFIX=str(args.prefix.resolve()), WINEDEBUG=env.get("WINEDEBUG", "-all"),
               WINEDLLOVERRIDES="mscoree,mshtml=;d3d9=n;d3dx9_43,d3dcompiler_43=b")
    command = [args.wine_build.resolve() / "loader/wine", out / "client.exe",
               windows(out / "d3d9.dll"), windows(out / "service.dll"),
               windows(args.backend64.resolve()), windows(args.d3dx.resolve()),
               windows(out / "texture.bmp")]
    if args.fullscreen:
        command.append("--fullscreen")
    if args.source_effect:
        command.append("--source-effect")
    output = run(command, "smoke", env)
    rows = re.findall(r"PW_GAME_SMOKE cycle=(\d) effect=1 mesh=1 texture=1 restored=1 pixel=([0-9a-f]+) present=1 status=0", output)
    assert rows == [(str(i), "ffff0000") for i in range(3)], rows
    receipt["cycles"] = rows
    receipt["status"] = "pass"
    receipt["scope"] = "Three real DLL/D3DX effect, mesh, texture, indexed draw, pixel readback, state restoration, Reset/Present and clean-exit cycles; not a GTA SA game result."
finally:
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps({"status": receipt["status"], "receipt": str(out / "receipt.json")}))
