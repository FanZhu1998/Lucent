"""Tests of research/tools/chart_reference.py (the independent colour reference, ADR 0020).

Synthetic data only, except the checks of the committed fixtures/qualification/charts.json; no
camera fixture is needed. Run with the research environment:

    research\\.venv\\Scripts\\python.exe -m unittest research.tests.test_chart_reference -v
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import chart_reference as cr  # noqa: E402


def exact_xyz(reference: dict, patch_ids: list[str], exposure: float) -> np.ndarray:
    """XYZ (D65-relative) whose evaluation reproduces the reference exactly."""
    lab = cr.reference_lab(reference, patch_ids)
    return exposure * cr.bradford(cr.lab_to_xyz(lab, cr.D50), cr.D50, cr.D65)


class Evaluation(unittest.TestCase):
    def setUp(self) -> None:
        self.charts = cr.load_charts(cr.DEFAULT_CHARTS)

    def test_the_published_colours_measure_zero(self) -> None:
        for fixture in self.charts["fixtures"]:
            reference = self.charts["references"][fixture["reference"]]
            ids = list(reference["patches"])
            result = cr.evaluate(exact_xyz(reference, ids, 0.37), ids, cr.reference_lab(reference, ids),
                                 fixture["normalisation_patch"], cr.D65)
            self.assertLess(result["as_shot"]["max"], 1e-6, fixture["id"])
            self.assertLess(result["chart_balanced"]["max"], 1e-6, fixture["id"])
            self.assertAlmostEqual(result["normalisation_scale"], 1 / 0.37, places=9)

    def test_chart_balancing_removes_a_white_balance_cast(self) -> None:
        reference = self.charts["references"]["xrite-colorchecker24-after-2014"]
        ids = list(reference["patches"])
        illuminant_a = cr.xy_to_xyz((0.4476, 0.4074))
        cast = cr.bradford(exact_xyz(reference, ids, 2.0), cr.D65, illuminant_a)
        result = cr.evaluate(cast, ids, cr.reference_lab(reference, ids), "C4", cr.D65)
        self.assertGreater(result["as_shot"]["mean"], 5.0)
        self.assertLess(result["chart_balanced"]["max"], 1e-6)

    def test_statistics_use_numpys_percentile(self) -> None:
        self.assertEqual(cr.stats(np.arange(1, 11, dtype=float)), {"mean": 5.5, "p90": 9.1, "max": 10.0})


class LibRawMatrix(unittest.TestCase):
    def test_white_balanced_neutral_maps_to_the_d65_white(self) -> None:
        cam_xyz = np.array([[0.6347, -0.0479, -0.0972], [-0.8297, 1.5954, 0.2480], [-0.1968, 0.2131, 0.7649],
                            [0.0, 0.0, 0.0]])
        xyz = cr.libraw_camera_to_xyz(cam_xyz) @ np.ones(3)
        np.testing.assert_allclose(xyz, cr.LIBRAW_XYZ_RGB.sum(axis=1), rtol=1e-12)
        self.assertAlmostEqual(xyz[1], 1.0, places=5)


class CommittedCharts(unittest.TestCase):
    def test_every_reference_patch_has_a_box_inside_its_frame(self) -> None:
        charts = cr.load_charts(cr.DEFAULT_CHARTS)
        manifest = {f["id"] for f in json.loads(cr.DEFAULT_MANIFEST.read_text(encoding="utf-8"))["fixtures"]}
        for fixture in charts["fixtures"]:
            self.assertIn(fixture["id"], manifest)
            reference = charts["references"][fixture["reference"]]
            self.assertEqual(reference["illuminant"], "D50")
            self.assertEqual(set(fixture["patches"]), set(reference["patches"]), fixture["id"])
            width, height = fixture["frame"]["size"]
            for x, y, w, h in fixture["patches"].values():
                self.assertTrue(0 <= x and 0 <= y and x + w <= width and y + h <= height, fixture["id"])
            self.assertIn("independent_measurement", fixture)

    def test_the_layout_is_stable(self) -> None:
        text = cr.DEFAULT_CHARTS.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertEqual(cr.format_charts(json.loads(text)) + "\n", text)


class Locate(unittest.TestCase):
    def test_boxes_are_centred_on_a_synthetic_chart(self) -> None:
        from PIL import Image

        image = np.full((400, 600, 3), 0.05)
        centres = {}
        colours = [(0.8, 0.2, 0.2), (0.2, 0.8, 0.2), (0.2, 0.2, 0.8), (0.7, 0.7, 0.2), (0.6, 0.6, 0.6), (0.3, 0.3, 0.3)]
        for k, (r, c) in enumerate([(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2)]):
            cx, cy = 150 + 150 * c, 120 + 160 * r
            image[cy - 50:cy + 50, cx - 50:cx + 50] = colours[k]
            centres[f"{'AB'[r]}{c + 1}"] = (cx, cy)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "chart.png"
            Image.fromarray((np.clip(image, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)).save(path)
            out = Path(tmp) / "out.json"
            stdout = sys.stdout
            try:
                with out.open("w", encoding="utf-8") as handle:
                    sys.stdout = handle
                    code = cr.main(["locate", "--image", str(path), "--rows", "2", "--cols", "3",
                                    "--ids-grid", "A1,A2,A3;B1,B2,B3", "--corners", "146,117 454,123 148,283 446,277"])
            finally:
                sys.stdout = stdout
            self.assertEqual(code, 0)
            boxes = json.loads(out.read_text(encoding="utf-8"))["boxes"]
        for pid, (cx, cy) in centres.items():
            x, y, w, h = boxes[pid]
            self.assertEqual((w, h), (50, 50))
            self.assertLessEqual(abs(x + w / 2 - cx), 1.5, pid)
            self.assertLessEqual(abs(y + h / 2 - cy), 1.5, pid)


if __name__ == "__main__":
    unittest.main()
