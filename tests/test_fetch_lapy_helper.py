# SPDX-License-Identifier: LGPL-2.1-or-later
import unittest
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.fetch_lapy_helper import select_latest_release, validate_helper_manifest


class SelectLatestReleaseTests(unittest.TestCase):
    def test_selects_newest_published_prerelease(self):
        releases = [
            {"tag_name": "v1.0.0", "published_at": "2026-09-29T10:00:00Z"},
            {"tag_name": "v1.1.0-rc1", "published_at": "2026-10-02T10:00:00Z",
             "prerelease": True},
            {"tag_name": "v1.2.0-draft", "created_at": "2026-10-03T10:00:00Z",
             "draft": True},
        ]

        self.assertEqual(select_latest_release(releases)["tag_name"], "v1.1.0-rc1")

    def test_fails_if_repository_has_no_published_release(self):
        with self.assertRaisesRegex(ValueError, "no published releases"):
            select_latest_release([{"tag_name": "draft", "draft": True}])


class HelperManifestTests(unittest.TestCase):
    def test_requires_exact_helper_identity_digests_and_retry_feature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / "lapy.elf"
            protocol = root / "lapy_elevation_protocol.h"
            elf.write_bytes(b"tested helper")
            protocol.write_bytes(b"wire ABI")
            import hashlib
            manifest = {
                "mode": "elf-helper",
                "target_title": "PPSA99995",
                "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
                "protocol_sha256": hashlib.sha256(protocol.read_bytes()).hexdigest(),
                "features": ["root_layout_probe_retry"],
            }
            validate_helper_manifest(manifest, elf, protocol, "PPSA99995")
            with self.assertRaisesRegex(ValueError, "root_layout_probe_retry"):
                validate_helper_manifest({**manifest, "features": []}, elf,
                                         protocol, "PPSA99995")
            with self.assertRaisesRegex(ValueError, "manifest, title or digest"):
                validate_helper_manifest({**manifest, "target_title": "PPSA00000"},
                                         elf, protocol, "PPSA99995")


if __name__ == "__main__":
    unittest.main()
