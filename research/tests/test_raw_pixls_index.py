"""Offline tests for research/tools/raw_pixls_index.py (synthetic index data, no network)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import raw_pixls_index as rpi  # noqa: E402

SHA_R5 = "21430c36387efe65bd09ac0fcd724bfd959d93dd391ec811a1b741c8663becb5"
SHA_NC = "d7bbb0740fd55a6cddae193bb03c873c1b774c10c6f4d2160d843460d5436fa4"
CC0 = "<a href='https:\\/\\/creativecommons.org\\/publicdomain\\/zero\\/1.0\\/' title='Creative Commons 0 - Public Domain' class='cc'>co<\\/a>"
NC = ("<a href='http:\\/\\/creativecommons.org\\/licenses\\/by-nc-sa\\/4.0\\/' title='Creative Commons - Attribution, "
      "Non-Commercial, ShareAlike 4.0' class='cc'>cbna<\\/a>")


def _row(sample_id, make, model, mode, remark, license_html, nice, sha, size):
    raw = (f"<a href='https:\\/\\/raw.pixls.us\\/getfile.php\\/{sample_id}\\/nice\\/{nice}'>{nice}<\\/a>"
           f"<div class='checksumdata'><span title='sha256 Checksum'>{sha}<\\/span>&nbsp;({size}MB)<\\/div>")
    return [make, model, mode, 45.0, remark, license_html, "2021-05-19", raw, ""]


# The real endpoint escapes "/" as "\/" inside JSON strings; json.loads undoes that.
INDEX_JSON = json.dumps({"data": [
    _row(4694, "Canon", "EOS R5", "3:2", "RAW no dualpixel", CC0, "Canon - EOS R5 - 3:2.CR3", SHA_R5, "38.75"),
    _row(133, "Olympus", "C8080WZ", "12bit (4:3)", "Import", NC, "Olympus - C8080WZ - 12bit (4:3).ORF", SHA_NC, "11.53"),
]}).replace("\\\\/", "\\/")
FILELIST = (
    f"{SHA_R5} *Canon/EOS R5/Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3\n"
    f"{SHA_NC} *Olympus/C8080WZ/RAW_OLYMPUS_C8080.ORF\n"
)


class ParseTest(unittest.TestCase):
    def setUp(self):
        self.samples = rpi.parse_index(INDEX_JSON, rpi.parse_filelist(FILELIST))

    def test_rows_are_joined_with_the_mirror(self):
        r5 = self.samples[0]
        self.assertEqual(r5.sample_id, 4694)
        self.assertEqual(r5.license_spdx, "CC0-1.0")
        self.assertEqual(r5.published_sha256, SHA_R5)
        self.assertEqual(r5.approx_size_mb, 38.75)
        self.assertEqual(r5.getfile_url, "https://raw.pixls.us/getfile.php/4694/nice/Canon%20-%20EOS%20R5%20-%203%3A2.CR3")
        self.assertEqual(
            r5.mirror_url, "https://raw.pixls.us/data/Canon/EOS%20R5/Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3"
        )
        self.assertEqual(r5.source_page, "https://raw.pixls.us/data/Canon/EOS%20R5/")
        self.assertEqual(self.samples[1].license_spdx, "CC-BY-NC-SA-4.0")

    def test_draft_entry_uses_safe_original_name(self):
        entry = rpi.draft_entry(self.samples[0])
        self.assertEqual(entry["file_name"], "Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3")
        self.assertEqual(entry["format"], "CR3")
        self.assertEqual(entry["raw_pixls_id"], 4694)
        self.assertNotIn("sha256", entry)

    def test_draft_entry_sanitises_unsafe_names(self):
        sample = rpi.parse_index(INDEX_JSON, {})[0]  # no mirror path: falls back to the display name
        entry = rpi.draft_entry(sample)
        self.assertRegex(entry["file_name"], rpi.SAFE_FILE_NAME)
        self.assertEqual(entry["file_name"], "Canon_-_EOS_R5_-_3_2.CR3")
        self.assertNotIn("mirror_url", entry)

    def test_malformed_inputs_raise(self):
        with self.assertRaises(rpi.IndexError_):
            rpi.parse_filelist("not a checksum line\n")
        with self.assertRaises(rpi.IndexError_):
            rpi.parse_index("{}", {})
        with self.assertRaises(rpi.IndexError_):
            rpi.parse_index(json.dumps({"data": [["a", "b", "c", 1, "", "", "", "<b>no link</b>"]]}), {})


class VerifyManifestTest(unittest.TestCase):
    def setUp(self):
        self.samples = rpi.parse_index(INDEX_JSON, rpi.parse_filelist(FILELIST))
        self.entry = rpi.draft_entry(self.samples[0])
        self.entry.update(id="canon-eos-r5-cr3-raw", sha256=SHA_R5)

    def verify(self, *entries):
        return rpi.verify_manifest({"fixtures": list(entries)}, self.samples)

    def test_consistent_entry_passes(self):
        self.assertEqual(self.verify(self.entry), [])

    def test_detects_problems(self):
        cases = {
            "sha256 ": dict(sha256="0" * 64),
            "no sha256": dict(sha256=None),
            "no longer exists": dict(raw_pixls_id=99999),
            "no raw_pixls_id": dict(raw_pixls_id=None),
            "does not point": dict(url="https://example.com/x.CR3"),
            "make/model": dict(model="EOS R6"),
            "only CC0-1.0": dict(license="CC-BY-4.0"),
        }
        for expected, change in cases.items():
            with self.subTest(expected):
                entry = dict(self.entry, **change)
                problems = self.verify({k: v for k, v in entry.items() if v is not None})
                self.assertTrue(any(expected in p for p in problems), problems)

    def test_non_cc0_archive_licence_is_reported(self):
        entry = rpi.draft_entry(self.samples[1])
        entry.update(id="olympus", sha256=SHA_NC, license="CC0-1.0")
        self.assertTrue(any("CC-BY-NC-SA-4.0" in p for p in self.verify(entry)))


class CommittedManifestTest(unittest.TestCase):
    """Structural checks of fixtures/manifest.json that need no network."""

    def test_manifest_is_complete_and_consistent(self):
        manifest = json.loads(rpi.DEFAULT_MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "lucent.fixture-manifest")
        self.assertEqual(manifest["schema_version"], 1)
        self.assertIn("CC0", manifest["license_note"])
        ids = [f["id"] for f in manifest["fixtures"]]
        self.assertEqual(len(ids), len(set(ids)))
        smoke_bytes = 0
        for fixture in manifest["fixtures"]:
            with self.subTest(fixture=fixture["id"]):
                self.assertRegex(fixture["id"], r"^[a-z0-9]+(-[a-z0-9]+)*$")
                self.assertIn(fixture["tier"], ("smoke", "qualification"))
                self.assertEqual(fixture["license"], "CC0-1.0")
                self.assertRegex(fixture["sha256"], r"^[0-9a-f]{64}$")
                self.assertRegex(fixture["file_name"], rpi.SAFE_FILE_NAME)
                self.assertTrue(fixture["url"].startswith("https://raw.pixls.us/getfile.php/"))
                self.assertGreater(fixture["size_bytes"], 0)
                for key in ("make", "model", "format", "mode", "source_page", "notes"):
                    self.assertTrue(fixture[key], key)
                if fixture["tier"] == "smoke":
                    smoke_bytes += fixture["size_bytes"]
        smoke_count = sum(1 for f in manifest["fixtures"] if f["tier"] == "smoke")
        self.assertTrue(4 <= smoke_count <= 6)
        self.assertLess(smoke_bytes, 250 * 1000 * 1000)


if __name__ == "__main__":
    unittest.main()
