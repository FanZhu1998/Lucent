"""Regression tests for the Image-Adaptive-3DLUT reproduction (research/lut/tools/reproduce_demo.py).

Need the pinned checkout under research/third_party (skipped without it). The output-hash test
also needs the environment recorded in results/demo_reproduction.json (same torch build, CPU
capability and thread count), because PyTorch's convolution results depend on the thread count;
it is skipped, with the reason, elsewhere.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import lut_common as lc  # noqa: E402

import numpy as np  # noqa: E402
import torch  # noqa: E402

RESULTS = lc.RESULTS_DIR / "demo_reproduction.json"


def checkout_or_skip() -> Path:
    checkout = lc.checkout_path()
    try:
        lc.verify_checkout(checkout)
    except SystemExit as e:
        raise unittest.SkipTest(f"pinned checkout unavailable: {e}")
    return checkout


class DemoPatch(unittest.TestCase):
    def test_exactly_the_documented_substitutions(self):
        import reproduce_demo as rd

        checkout = checkout_or_skip()
        with tempfile.TemporaryDirectory() as tmp:
            patch = rd.patched_demo(checkout, Path(tmp) / "demo.py")
        removed = [ln[1:] for ln in patch["diff"] if ln.startswith("-") and not ln.startswith("---")]
        added = [ln[1:] for ln in patch["diff"] if ln.startswith("+") and not ln.startswith("+++")]
        expected = rd.README_SUBSTITUTIONS + rd.CPU_SUBSTITUTIONS
        self.assertEqual(sorted(removed), sorted(old for old, _ in expected))
        self.assertEqual(sorted(added), sorted(new for _, new in expected))


class Predictor(unittest.TestCase):
    def test_numpy_classifier_matches_torch(self):
        import classifier_numpy as cnp
        import upstream_model as um

        model = um.load(checkout_or_skip())
        img = torch.rand(1, 3, 150, 230, generator=torch.Generator().manual_seed(3))
        with torch.no_grad():
            ref = model.classifier(img).squeeze().double().numpy()
        got, trace = cnp.forward(model.classifier.state_dict(), img[0].numpy())
        self.assertEqual(trace[-1]["out"], [3, 1, 1])
        np.testing.assert_allclose(got, ref, rtol=0, atol=2e-5)


class CommittedResults(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not RESULTS.exists():
            raise unittest.SkipTest("results/demo_reproduction.json not generated")
        cls.results = json.loads(RESULTS.read_text(encoding="utf-8"))

    def test_checkout_files_match_recorded_hashes(self):
        checkout = checkout_or_skip()
        for name, rec in self.results["checkpoints"].items():
            self.assertEqual(lc.sha256_file(checkout / "pretrained_models" / "sRGB" / name), rec["file_sha256"])
        demo = self.results["images"]["a1629.jpg"]["input"]
        self.assertEqual(lc.sha256_file(checkout / "demo_images" / "sRGB" / "a1629.jpg"), demo["file_sha256"])

    def test_demo_reproduces_recorded_output(self):
        import reproduce_demo as rd
        import trilinear_torch as tt

        checkout = checkout_or_skip()
        env, now = self.results["environment"], lc.environment()
        keys = ("torch", "torch_cpu_capability", "torch_num_threads", "pillow", "libjpeg_turbo")
        if any(env[k] != now[k] for k in keys):
            self.skipTest("environment differs from the recorded run: " +
                          ", ".join(f"{k} {env[k]} -> {now[k]}" for k in keys if env[k] != now[k]))
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "demo.py"
            rd.patched_demo(checkout, script)
            run = rd.run_demo(checkout, script, "demo_images", "a1629.jpg", tt.ExtensionApi(), Path(tmp) / "out")
        recorded = self.results["images"]["a1629.jpg"]["demo"]
        self.assertEqual(lc.array_record(run["call"]["output"])["sha256"], recorded["output_float32"]["sha256"])
        self.assertEqual(lc.array_record(run["globals"]["ndarr"])["sha256"], recorded["output_uint8"]["sha256"])
        lc.assert_checkout_unchanged(checkout)


if __name__ == "__main__":
    unittest.main()
