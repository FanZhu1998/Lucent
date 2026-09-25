"""Tests for research/tools/compare_probe.py (synthetic reports; no fixtures needed)."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import compare_probe  # noqa: E402

BASE_PARITY = {
    "raw_width": 6080,
    "raw_height": 4032,
    "width": 6000,
    "height": 4000,
    "top_margin": 32,
    "left_margin": 80,
    "iwidth": 6000,
    "iheight": 4000,
    "flip": 0,
    "num_colors": 3,
    "color_desc": "RGBG",
    "raw_pattern": [[0, 1], [3, 2]],
    "black_level_per_channel": [512, 512, 512, 512],
    "white_level": 16383,
    "camera_white_level_per_channel": None,
    "camera_whitebalance": [2.0390625, 1.0, 1.5, 0.0],
    "daylight_whitebalance": [2.1, 0.95, 1.4, 0.0],
    "rgb_xyz_matrix": [[0.7, -0.1, -0.1], [-0.4, 1.2, 0.3], [-0.1, 0.2, 0.6], [0.0, 0.0, 0.0]],
    "color_matrix": [[0.0] * 4, [0.0] * 4, [0.0] * 4],
    "raw_type": "flat",
    "raw_data_sha256": "ab" * 32,
}


def make_report(parity=BASE_PARITY, **overrides):
    report = {
        "schema": "lucent.probe-report",
        "schema_version": 1,
        "file_name": "IMG_0001.CR3",
        "file_size": 1234,
        "file_sha256": "cd" * 32,
        "generator": {"name": "test"},
        "status": "ok" if parity is not None else "error",
        "parity": copy.deepcopy(parity),
    }
    report.update(overrides)
    return report


class CompareProbeTest(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)

    def tearDown(self):
        self._dir.cleanup()

    def write(self, name, document):
        path = self.dir / name
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def run_compare(self, lucent, reference, *extra):
        out, err = io.StringIO(), io.StringIO()
        args = [str(self.write("lucent.json", lucent)), str(self.write("reference.json", reference)), *extra]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = compare_probe.main(args)
        return code, out.getvalue() + err.getvalue()

    def with_parity(self, **changes):
        parity = copy.deepcopy(BASE_PARITY)
        parity.update(changes)
        return make_report(parity)

    # ---- matches --------------------------------------------------------------------------

    def test_identical_reports_match(self):
        code, out = self.run_compare(make_report(), make_report())
        self.assertEqual(code, 0, out)
        self.assertIn("21 fields compared, 0 mismatched", out)

    def test_float_within_relative_tolerance_matches(self):
        # 2.0390625 * (1 + 5e-7) differs by less than rel 1e-6.
        lucent = self.with_parity(camera_whitebalance=[2.0390625 * (1 + 5e-7), 1.0, 1.5, 0.0])
        code, out = self.run_compare(lucent, make_report())
        self.assertEqual(code, 0, out)

    def test_float_near_zero_uses_absolute_tolerance(self):
        lucent = self.with_parity(camera_whitebalance=[2.0390625, 1.0, 1.5, 5e-10])
        self.assertEqual(self.run_compare(lucent, make_report())[0], 0)

    def test_integral_number_accepted_in_float_field(self):
        lucent = self.with_parity(camera_whitebalance=[2.0390625, 1, 1.5, 0])
        self.assertEqual(self.run_compare(lucent, make_report())[0], 0)

    def test_matching_non_finite_markers(self):
        a = self.with_parity(daylight_whitebalance=["nan", 1.0, "inf", 0.0])
        self.assertEqual(self.run_compare(a, copy.deepcopy(a))[0], 0)

    def test_raw_hash_absent_on_one_side_is_not_compared(self):
        lucent = make_report({k: v for k, v in BASE_PARITY.items() if k != "raw_data_sha256"})
        code, out = self.run_compare(lucent, make_report())
        self.assertEqual(code, 0, out)
        self.assertIn("not compared", out)

    def test_both_sides_without_parity_is_consistent(self):
        failed = make_report(None, error={"stage": "unpack", "type": "LibRawFileUnsupportedError"})
        code, out = self.run_compare(failed, copy.deepcopy(failed))
        self.assertEqual(code, 0, out)
        self.assertIn("consistent", out)

    # ---- mismatches -----------------------------------------------------------------------

    def test_integer_difference_is_a_mismatch(self):
        code, out = self.run_compare(self.with_parity(white_level=16384), make_report())
        self.assertEqual(code, 1)
        self.assertIn("MISMATCH: white_level: 16384 vs 16383", out)

    def test_float_outside_tolerance_is_a_mismatch(self):
        lucent = self.with_parity(camera_whitebalance=[2.0390625 * (1 + 2e-6), 1.0, 1.5, 0.0])
        code, out = self.run_compare(lucent, make_report())
        self.assertEqual(code, 1)
        self.assertIn("camera_whitebalance: [0]", out)

    def test_float_given_for_integer_field_is_a_mismatch(self):
        self.assertEqual(self.run_compare(self.with_parity(flip=0.0), make_report())[0], 1)

    def test_bool_is_not_an_integer(self):
        self.assertEqual(self.run_compare(self.with_parity(num_colors=True), self.with_parity(num_colors=1))[0], 1)

    def test_pattern_shape_difference_is_a_mismatch(self):
        lucent = self.with_parity(raw_pattern=[[0, 1, 0, 1], [3, 2, 3, 2], [0, 1, 0, 1], [3, 2, 3, 2]])
        code, out = self.run_compare(lucent, make_report())
        self.assertEqual(code, 1)
        self.assertIn("shape", out)

    def test_null_versus_value_is_a_mismatch(self):
        lucent = self.with_parity(camera_white_level_per_channel=[16383] * 4)
        self.assertEqual(self.run_compare(lucent, make_report())[0], 1)

    def test_null_in_non_nullable_field_is_a_mismatch(self):
        a = self.with_parity(white_level=None)
        self.assertEqual(self.run_compare(a, copy.deepcopy(a))[0], 1)

    def test_missing_contract_field_is_a_mismatch(self):
        lucent = make_report({k: v for k, v in BASE_PARITY.items() if k != "flip"})
        code, out = self.run_compare(lucent, make_report())
        self.assertEqual(code, 1)
        self.assertIn("missing from lucent", out)

    def test_different_raw_hash_is_a_mismatch(self):
        self.assertEqual(self.run_compare(self.with_parity(raw_data_sha256="ef" * 32), make_report())[0], 1)

    def test_require_raw_hash_fails_when_one_side_lacks_it(self):
        lucent = make_report({k: v for k, v in BASE_PARITY.items() if k != "raw_data_sha256"})
        self.assertEqual(self.run_compare(lucent, make_report(), "--require-raw-hash")[0], 1)

    def test_extra_field_is_compared_and_flagged(self):
        code, out = self.run_compare(self.with_parity(new_thing=3), self.with_parity(new_thing=4))
        self.assertEqual(code, 1)
        self.assertIn("EXTRA MISMATCH", out)
        code, out = self.run_compare(self.with_parity(new_float=1.0), self.with_parity(new_float=1.0 + 1e-9))
        self.assertEqual(code, 0, out)
        self.assertIn("extra ok", out)

    def test_different_files_are_not_compared(self):
        code, out = self.run_compare(make_report(file_size=99), make_report())
        self.assertEqual(code, 1)
        self.assertIn("different files", out)
        code, _ = self.run_compare(make_report(file_sha256="00" * 32), make_report())
        self.assertEqual(code, 1)

    def test_one_side_without_parity_is_a_mismatch(self):
        failed = make_report(None, error={"stage": "unpack", "type": "LibRawFileUnsupportedError"})
        code, out = self.run_compare(failed, make_report())
        self.assertEqual(code, 1)
        self.assertIn("lucent has no parity block", out)

    # ---- allowances -----------------------------------------------------------------------

    def allowances(self, *entries, **top):
        document = {"schema": "lucent.parity-allowances", "schema_version": 1, "allowances": list(entries)}
        document.update(top)
        return str(self.write("allow.json", document))

    def test_tolerance_allowance_accepts_documented_difference(self):
        lucent = self.with_parity(daylight_whitebalance=[2.1 * (1 + 5e-5), 0.95, 1.4, 0.0])
        allow = self.allowances(
            {"field": "daylight_whitebalance", "action": "tolerance", "rel_tol": 1e-4, "reason": "test"}
        )
        code, out = self.run_compare(lucent, make_report(), "--allowances", allow)
        self.assertEqual(code, 0, out)
        self.assertIn("allowed", out)
        self.assertIn("allowance[0] used", out)

    def test_tolerance_allowance_still_rejects_larger_difference(self):
        lucent = self.with_parity(daylight_whitebalance=[2.1 * 1.01, 0.95, 1.4, 0.0])
        allow = self.allowances(
            {"field": "daylight_whitebalance", "action": "tolerance", "rel_tol": 1e-4, "reason": "test"}
        )
        self.assertEqual(self.run_compare(lucent, make_report(), "--allowances", allow)[0], 1)

    def test_allowance_not_needed_is_reported_as_unused(self):
        allow = self.allowances({"field": "white_level", "action": "ignore", "reason": "test"})
        code, out = self.run_compare(make_report(), make_report(), "--allowances", allow)
        self.assertEqual(code, 0)
        self.assertIn("was not needed", out)

    def test_ignore_allowance_respects_file_patterns(self):
        lucent = self.with_parity(white_level=16000)
        matching = self.allowances(
            {"field": "white_level", "action": "ignore", "files": ["*.cr3"], "reason": "test"}
        )
        self.assertEqual(self.run_compare(lucent, make_report(), "--allowances", matching)[0], 0)
        other = self.allowances({"field": "white_level", "action": "ignore", "files": ["*.NEF"], "reason": "test"})
        self.assertEqual(self.run_compare(lucent, make_report(), "--allowances", other)[0], 1)

    def test_invalid_allowance_files_are_usage_errors(self):
        cases = [
            {"field": "white_level", "action": "ignore"},  # no reason
            {"field": "white_level", "action": "ignore", "reason": "  "},
            {"field": "white_level", "action": "tolerance", "rel_tol": 1e-3, "reason": "int field"},
            {"field": "camera_whitebalance", "action": "tolerance", "rel_tol": -1, "reason": "negative"},
            {"field": "camera_whitebalance", "action": "skip", "reason": "unknown action"},
            {"field": "camera_whitebalance", "action": "ignore", "reason": "x", "typo": 1},
        ]
        for entry in cases:
            with self.subTest(entry=entry):
                code, out = self.run_compare(make_report(), make_report(), "--allowances", self.allowances(entry))
                self.assertEqual(code, 2, out)
        code, _ = self.run_compare(make_report(), make_report(), "--allowances", self.allowances(schema_version=2))
        self.assertEqual(code, 2)

    # ---- schema handling ------------------------------------------------------------------

    def test_newer_schema_version_is_rejected(self):
        code, out = self.run_compare(make_report(schema_version=2), make_report())
        self.assertEqual(code, 2)
        self.assertIn("not supported", out)

    def test_wrong_schema_and_bad_json_are_rejected(self):
        self.assertEqual(self.run_compare(make_report(schema="other"), make_report())[0], 2)
        bad = self.dir / "bad.json"
        bad.write_text("{not json", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(compare_probe.main([str(bad), str(self.write("ok.json", make_report()))]), 2)
            self.assertEqual(compare_probe.main([str(self.dir / "missing.json"), str(bad)]), 2)


class FloatsCloseTest(unittest.TestCase):
    def test_limits(self):
        close = compare_probe.floats_close
        self.assertTrue(close(1.0, 1.0 + 1e-6, 1e-6, 1e-9)[0])
        self.assertFalse(close(1.0, 1.0 + 1.1e-6, 1e-6, 1e-9)[0])
        self.assertTrue(close(0.0, 1e-9, 1e-6, 1e-9)[0])
        self.assertFalse(close(0.0, 2e-9, 1e-6, 1e-9)[0])
        self.assertFalse(close("nan", 1.0, 1e-6, 1e-9)[0])
        self.assertFalse(close("bogus", "bogus", 1e-6, 1e-9)[0])


if __name__ == "__main__":
    unittest.main()
