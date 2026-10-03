#!/usr/bin/env python3
"""Fetch the one-shot Lapy helper from the repository's latest GitHub release."""
import argparse
import json
import hashlib
from pathlib import Path
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def fetch(url: str, destination: Path) -> None:
    request = Request(url, headers={"User-Agent": "prospero-win-build/1"})
    with urlopen(request, timeout=30) as response, destination.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            output.write(chunk)


def select_latest_release(releases: list[dict]) -> dict:
    published = [release for release in releases if not release.get("draft")]
    if not published:
        raise ValueError("the repository has no published releases")
    return max(published, key=lambda release:
               release.get("published_at") or release.get("created_at") or "")


def validate_helper_manifest(manifest: dict, elf: Path, protocol: Path,
                             target_title: str) -> None:
    """Reject helpers that do not declare the safety behavior this title uses."""
    actual_elf_sha256 = hashlib.sha256(elf.read_bytes()).hexdigest()
    actual_protocol_sha256 = hashlib.sha256(protocol.read_bytes()).hexdigest()
    if (manifest.get("mode") != "elf-helper" or
            manifest.get("target_title") != target_title or
            manifest.get("elf_sha256") != actual_elf_sha256 or
            manifest.get("protocol_sha256") != actual_protocol_sha256):
        raise ValueError("latest Lapy release manifest, title or digest does not match")
    features = manifest.get("features")
    if not isinstance(features, list) or "root_layout_probe_retry" not in features:
        raise ValueError(
            "latest Lapy helper lacks required feature: root_layout_probe_retry")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, help="GitHub owner/repository")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo):
        parser.error("--repo must be owner/repository")

    api = f"https://api.github.com/repos/{args.repo}/releases?per_page=100"
    try:
        request = Request(api, headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "prospero-win-build/1",
        })
        with urlopen(request, timeout=30) as response:
            release = select_latest_release(json.load(response))
        assets = {asset.get("name"): asset.get("browser_download_url")
                  for asset in release.get("assets", [])}
        required = ("lapy.elf", "lapy-manifest.json")
        missing = [name for name in required if not assets.get(name)]
        if missing:
            raise ValueError(
                f"latest release {release.get('tag_name', '?')} of {args.repo} "
                f"does not publish required assets: {', '.join(missing)}")
        args.out.mkdir(parents=True, exist_ok=True)
        for name in required:
            fetch(assets[name], args.out / name)
        metadata = {
            "repository": args.repo,
            "tag_name": release.get("tag_name"),
            "release_url": release.get("html_url"),
            "published_at": release.get("published_at"),
        }
        (args.out / "release.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        print(f"Fetched {args.repo}@{metadata['tag_name']} ({metadata['release_url']})")
        return 0
    except (HTTPError, URLError, TimeoutError, OSError, ValueError,
            json.JSONDecodeError) as error:
        print(f"Could not fetch latest Lapy helper release: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
