# SPDX-License-Identifier: LGPL-2.1-or-later
import unittest
from pathlib import Path
import sys
import tempfile
import hashlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.fetch_lapy_helper import validate_helper_manifest, verify_pinned_elf


class PinnedElfTests(unittest.TestCase):
    def test_accepts_the_pinned_bytes_in_either_case(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "lapy.elf"
            elf.write_bytes(b"pinned helper")
            digest = hashlib.sha256(b"pinned helper").hexdigest()
            verify_pinned_elf(elf, digest)
            verify_pinned_elf(elf, digest.upper())

    def test_rejects_other_bytes_and_names_both_digests(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "lapy.elf"
            elf.write_bytes(b"newer helper")
            pinned = hashlib.sha256(b"pinned helper").hexdigest()
            actual = hashlib.sha256(b"newer helper").hexdigest()
            with self.assertRaisesRegex(ValueError, f"{actual}.*{pinned}"):
                verify_pinned_elf(elf, pinned)

    def test_rejects_an_empty_download(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / "lapy.elf"
            elf.write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "the pin is"):
                verify_pinned_elf(elf, hashlib.sha256(b"pinned helper").hexdigest())


class HelperManifestTests(unittest.TestCase):
    def test_requires_exact_helper_identity_digests_and_retry_feature(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = root / "lapy.elf"
            protocol = root / "lapy_elevation_protocol.h"
            elf.write_bytes(b"tested helper")
            protocol.write_bytes(b"wire ABI")
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
