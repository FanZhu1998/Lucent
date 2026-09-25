"""Tests for research/tools/probe_reference.py.

Unit tests use a stand-in object with rawpy.RawPy's property names. The regression tests re-probe
cached fixtures (tools/fetch-fixtures.ps1) and compare with the committed fixtures/reference files;
they skip when a fixture is not downloaded.
"""

from __future__ import annotations

import contextlib
import enum
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import probe_reference  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "fixtures" / "manifest.json"
REFERENCE_DIR = REPO_ROOT / "fixtures" / "reference"


class FakeRawType(enum.Enum):
    Flat = 0
    Stack = 1


class FakeRaw:
    """Mimics the rawpy.RawPy properties read by build_parity."""

    def __init__(self, *, stack=False, raw_image=None):
        self.raw_type = FakeRawType.Stack if stack else FakeRawType.Flat
        self.sizes = SimpleNamespace(
            raw_width=4, raw_height=2, width=2, height=2, top_margin=0, left_margin=1,
            iwidth=2, iheight=2, flip=6,
        )
        self.num_colors = 3
        self.color_desc = b"RGBG"
        self.raw_pattern = None if stack else np.array([[0, 1], [3, 2]], dtype=np.uint8)
        self.black_level_per_channel = [512, 512, 512, 512]
        self.white_level = 16383
        self.camera_white_level_per_channel = None
        self.camera_whitebalance = [2.0390625, 1.0, float(np.float32(1.4)), 0.0]
        self.daylight_whitebalance = [float("nan"), 1.0, float("inf"), 0.0]
        self.rgb_xyz_matrix = np.arange(12, dtype=np.float32).reshape(4, 3) / 8
        self.color_matrix = np.zeros((3, 4), dtype=np.float32)
        if raw_image is None:
            raw_image = np.array([[1, 2, 3, 4], [5, 6, 7, 0xFFFF]], dtype=np.uint16)
        self.raw_image = raw_image


class BuildParityTest(unittest.TestCase):
    def test_field_order_and_values(self):
        parity = probe_reference.build_parity(FakeRaw(), decode=False)
        self.assertEqual(
            list(parity),
            [
                "raw_width", "raw_height", "width", "height", "top_margin", "left_margin", "iwidth",
                "iheight", "flip", "num_colors", "color_desc", "raw_pattern", "black_level_per_channel",
                "white_level", "camera_white_level_per_channel", "camera_whitebalance",
                "daylight_whitebalance", "rgb_xyz_matrix", "color_matrix", "raw_type",
            ],
        )
        self.assertEqual(parity["color_desc"], "RGBG")
        self.assertEqual(parity["raw_pattern"], [[0, 1], [3, 2]])
        self.assertEqual(parity["flip"], 6)
        self.assertEqual(parity["raw_type"], "flat")
        self.assertIsNone(parity["camera_white_level_per_channel"])
        # float32 values are widened exactly, not rounded to decimal.
        self.assertEqual(parity["camera_whitebalance"][2], float(np.float32(1.4)))
        self.assertEqual(parity["rgb_xyz_matrix"][3], [1.125, 1.25, 1.375])
        self.assertEqual(parity["daylight_whitebalance"][:3], ["nan", 1.0, "inf"])

    def test_decode_adds_raw_hash_for_flat_raws(self):
        raw = FakeRaw()
        parity = probe_reference.build_parity(raw, decode=True)
        expected = hashlib.sha256(raw.raw_image.astype("<u2").tobytes()).hexdigest()
        self.assertEqual(parity["raw_data_sha256"], expected)

    def test_raw_hash_is_little_endian_row_major(self):
        image = np.array([[0x0102, 0x0304], [0x0506, 0x0708]], dtype=">u2")  # big-endian input
        expected = hashlib.sha256(bytes([2, 1, 4, 3, 6, 5, 8, 7])).hexdigest()
        self.assertEqual(probe_reference.raw_buffer_sha256(image), expected)
        # A non-contiguous view hashes its logical samples, not the parent buffer.
        parent = np.array([[0x0102, 9, 0x0304], [0x0506, 9, 0x0708]], dtype="<u2")
        self.assertEqual(probe_reference.raw_buffer_sha256(parent[:, ::2]), expected)

    def test_raw_hash_rejects_stacked_data(self):
        with self.assertRaises(ValueError):
            probe_reference.raw_buffer_sha256(np.zeros((2, 2, 3), dtype=np.uint16))

    def test_stack_raw_has_null_pattern_and_no_hash(self):
        parity = probe_reference.build_parity(FakeRaw(stack=True), decode=True)
        self.assertEqual(parity["raw_type"], "stack")
        self.assertIsNone(parity["raw_pattern"])
        self.assertNotIn("raw_data_sha256", parity)

    def test_non_ascii_color_description_fails_loudly(self):
        raw = FakeRaw()
        raw.color_desc = b"RG\xffG"
        with self.assertRaises(UnicodeDecodeError):
            probe_reference.build_parity(raw, decode=False)


class HelpersTest(unittest.TestCase):
    def test_json_number(self):
        self.assertEqual(probe_reference.json_number(float("-inf")), "-inf")
        self.assertEqual(probe_reference.json_number(np.float32(0.5)), 0.5)

    def test_dumps_is_valid_deterministic_json(self):
        document = {"b": [[1, 2], [3, 4]], "a": {"x": [1.5, "nan"], "y": None, "z": {}}, "s": "é"}
        text = probe_reference.dumps(document)
        self.assertEqual(json.loads(text), document)
        self.assertEqual(text, probe_reference.dumps(json.loads(text)))
        self.assertIn('    [1, 2],\n', text)
        self.assertTrue(text.endswith("}\n"))

    def test_error_record_maps_libraw_codes(self):
        import rawpy

        record = probe_reference._error_record("unpack", rawpy.LibRawFileUnsupportedError(b"Unsupported"))
        self.assertEqual(record, {
            "stage": "unpack", "type": "LibRawFileUnsupportedError", "libraw_code": -2, "message": "Unsupported",
        })
        generic = probe_reference._error_record("open", rawpy.LibRawFatalError("boom"))
        self.assertIsNone(generic["libraw_code"])

    def test_parity_reference_drops_only_extras(self):
        report = {"schema": "x", "parity": {"a": 1}, "reference": {"b": 2}}
        self.assertEqual(probe_reference.parity_reference(report), {"schema": "x", "parity": {"a": 1}})


class ProbeFileTest(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)

    def tearDown(self):
        self._dir.cleanup()

    def test_missing_file_is_an_input_error(self):
        with self.assertRaises(probe_reference.ProbeInputError):
            probe_reference.probe_file(self.dir / "absent.CR3", decode=False)

    def test_manifest_hash_mismatch_is_an_input_error(self):
        path = self.dir / "file.NEF"
        path.write_bytes(b"not a raw file")
        with self.assertRaises(probe_reference.ProbeInputError):
            probe_reference.probe_file(path, decode=False, expected_sha256="00" * 32)

    def test_non_raw_file_is_recorded_as_libraw_error(self):
        path = self.dir / "garbage.CR3"
        path.write_bytes(bytes(range(256)) * 64)
        report = probe_reference.probe_file(path, decode=True, fixture_id="garbage")
        self.assertEqual(report["status"], "error")
        self.assertIsNone(report["parity"])
        self.assertEqual(report["error"]["stage"], "open")
        self.assertEqual(report["error"]["libraw_code"], -2)
        self.assertEqual(report["file_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(report["generator"]["name"], "rawpy-reference")

    def test_cli_rejects_mixed_inputs_and_reports_failures(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as raised:
            probe_reference.main([str(self.dir / "x.CR3"), "--manifest", str(MANIFEST)])
        self.assertEqual(raised.exception.code, 2)
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(probe_reference.main([str(self.dir / "absent.CR3")]), 1)
            self.assertEqual(probe_reference.main(["--manifest", str(MANIFEST), "--id", "no-such-id"]), 2)


def _manifest_entries():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))["fixtures"]


class ReferenceRegressionTest(unittest.TestCase):
    """Re-probes cached smoke fixtures and compares with the committed parity references."""

    def test_committed_references_cover_every_manifest_entry(self):
        ids = {entry["id"] for entry in _manifest_entries()}
        committed = {path.name[: -len(".parity.json")] for path in REFERENCE_DIR.glob("*.parity.json")}
        self.assertEqual(ids, committed)
        for entry in _manifest_entries():
            reference = json.loads((REFERENCE_DIR / f"{entry['id']}.parity.json").read_text(encoding="utf-8"))
            self.assertEqual(reference["file_sha256"], entry["sha256"], entry["id"])
            self.assertEqual(reference["file_size"], entry["size_bytes"], entry["id"])
            self.assertEqual(reference["file_name"], entry["file_name"], entry["id"])

    def test_smoke_fixtures_reproduce_committed_references(self):
        smoke = [entry for entry in _manifest_entries() if entry["tier"] == "smoke"]
        probed = 0
        for entry in smoke:
            path = MANIFEST.parent / "cache" / entry["id"] / entry["file_name"]
            with self.subTest(fixture=entry["id"]):
                if not path.is_file():
                    self.skipTest(f"{entry['id']} not downloaded; run tools/fetch-fixtures.ps1")
                committed = json.loads((REFERENCE_DIR / f"{entry['id']}.parity.json").read_text(encoding="utf-8"))
                if committed["generator"]["rawpy"] != probe_reference.rawpy.__version__:
                    self.skipTest("committed references were generated with a different rawpy version")
                report = probe_reference.probe_file(
                    path, decode=True, fixture_id=entry["id"], expected_sha256=entry["sha256"]
                )
                self.assertEqual(report["status"], committed["status"])
                self.assertEqual(report["parity"], committed["parity"])
                probed += 1
        if probed == 0:
            self.skipTest("no smoke fixture is downloaded; run tools/fetch-fixtures.ps1")


if __name__ == "__main__":
    unittest.main()
