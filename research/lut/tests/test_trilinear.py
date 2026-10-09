"""Tests for research/lut/tools/trilinear_numpy.py and trilinear_torch.py (synthetic data).

The extension tests need research/lut/build/trilinear_ext (build_trilinear_ext.ps1) and skip
without it. Every comparison between implementations is bit for bit: they perform the same
correctly rounded float32 operations in the same order.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import lut_common as lc  # noqa: E402

import numpy as np  # noqa: E402
import torch  # noqa: E402

import trilinear_numpy as tnp  # noqa: E402
import trilinear_torch as tt  # noqa: E402

DIM = 33
BS = lc.BINSIZE_MODELS_X(DIM)
# float32 inputs in [0, 1] where floor(x / BS) and fmod(x, BS) disagree (the index census in
# results/trilinear_parity.json found exactly these eight).
CENSUS_BITS = [0x3F080008, 0x3F180009, 0x3F28000A, 0x3F38000B, 0x3F48000C, 0x3F58000D, 0x3F68000E, 0x3F78000F]


def identity_lut() -> np.ndarray:
    g = (np.arange(DIM) / (DIM - 1)).astype(np.float32)
    return np.stack([
        np.broadcast_to(g[None, None, :], (DIM,) * 3),
        np.broadcast_to(g[None, :, None], (DIM,) * 3),
        np.broadcast_to(g[:, None, None], (DIM,) * 3),
    ]).astype(np.float32)


def random_lut(seed: int) -> np.ndarray:
    return np.random.default_rng(seed).uniform(-0.5, 1.5, (3, DIM, DIM, DIM)).astype(np.float32)


def edge_values() -> np.ndarray:
    bs = np.float32(BS)
    vals = [0.0, 1e-45, 1.0 / 255, 0.5, float(np.nextafter(np.float32(1), np.float32(0))), 1.0]
    for k in range(1, DIM - 1):
        x0 = np.float32(k * float(bs))
        vals += [float(x0), float(np.nextafter(x0, np.float32(0))), float(np.nextafter(x0, np.float32(2)))]
    vals += [float(np.uint32(b).view(np.float32)) for b in CENSUS_BITS]
    return np.asarray(sorted(set(vals)), dtype=np.float32)


def as_image(values: np.ndarray) -> np.ndarray:
    n = values.size
    return np.stack([values, np.roll(values, n // 3), values[::-1]]).reshape(3, 1, n).astype(np.float32)


def torch_forward(lut: np.ndarray, img: np.ndarray, binsize: float = BS) -> np.ndarray:
    return tt.forward(torch.from_numpy(lut), torch.from_numpy(img), binsize).numpy()


def same_bits(a: np.ndarray, b: np.ndarray) -> bool:
    return a.shape == b.shape and bool(np.array_equal(a.view(np.uint32), b.view(np.uint32)))


class NumpyTorchParity(unittest.TestCase):
    def test_random_inputs_and_luts(self):
        rng = np.random.default_rng(7)
        for seed in (1, 2):
            lut = random_lut(seed)
            img = rng.uniform(0.0, 1.0, (3, 64, 97)).astype(np.float32)
            self.assertTrue(same_bits(tnp.forward(lut, img, BS), torch_forward(lut, img)))

    def test_edge_values(self):
        img = as_image(edge_values())
        for lut in (identity_lut(), random_lut(3)):
            self.assertTrue(same_bits(tnp.forward(lut, img, BS), torch_forward(lut, img)))

    def test_legacy_binsize(self):
        lut, img = random_lut(4), as_image(edge_values())
        legacy = lc.BINSIZE_MODELS_LEGACY(DIM)
        self.assertTrue(same_bits(tnp.forward(lut, img, legacy), torch_forward(lut, img, legacy)))

    def test_eight_bit_values_match_torchvision_division(self):
        v = torch.arange(256, dtype=torch.uint8).to(torch.float32).div(255).numpy()
        self.assertTrue(same_bits(v, (np.arange(256, dtype=np.float32) / np.float32(255))))


class ExtensionParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ext = lc.load_extension()
        if cls.ext is None:
            raise unittest.SkipTest("CPU extension not built (research/lut/tools/build_trilinear_ext.ps1)")

    def run_ext(self, lut: np.ndarray, img: np.ndarray, binsize: float = BS) -> np.ndarray:
        x = torch.from_numpy(img).unsqueeze(0).contiguous()
        out = torch.empty_like(x)
        rc = self.ext.forward(torch.from_numpy(lut), x, out, DIM, DIM ** 3, binsize, x.shape[2], x.shape[3], 1)
        self.assertEqual(rc, 1)
        return out[0].numpy()

    def test_random_and_edge_inputs(self):
        rng = np.random.default_rng(11)
        for lut in (random_lut(5), identity_lut()):
            for img in (rng.uniform(0.0, 1.0, (3, 48, 80)).astype(np.float32), as_image(edge_values())):
                self.assertTrue(same_bits(self.run_ext(lut, img), tnp.forward(lut, img, BS)))

    def test_refuses_non_rgb(self):
        x = torch.zeros(1, 4, 2, 2)
        self.assertEqual(self.ext.forward(torch.from_numpy(identity_lut()), x, torch.empty_like(x),
                                          DIM, DIM ** 3, BS, 2, 2, 1), 0)


class ReferenceBehaviour(unittest.TestCase):
    def test_identity_round_trips_8bit_ramps(self):
        v = np.arange(256, dtype=np.float32) / np.float32(255)
        z = np.zeros_like(v)
        for ramp in ((v, v, v), (v, z, z), (z, v, z), (z, z, v)):
            img = np.stack(ramp).reshape(3, 1, 256).astype(np.float32)
            out = tnp.forward(identity_lut(), img, BS)
            self.assertLess(float(np.abs(out - img).max()), 2e-6)
            # demo_eval's quantisation, in float32: mul(255).add(0.5).clamp(0, 255), then truncation
            q = torch.from_numpy(out).mul(255).add(0.5).clamp(0, 255).to(torch.uint8).numpy()
            self.assertTrue(np.array_equal(q, np.round(img * 255).astype(np.uint8)))

    def test_white_is_not_exactly_the_top_node(self):
        # binsize = 1.000001/32 puts x = 1 at fraction 0.999968 of the top cell.
        out = tnp.forward(identity_lut(), np.ones((3, 1, 1), np.float32), BS)
        self.assertLess(float(out.max()), 1.0)
        self.assertGreater(float(out.min()), 0.999998)

    def test_census_values_jump_one_node(self):
        x = np.asarray([np.uint32(b).view(np.float32) for b in CENSUS_BITS], dtype=np.float32)
        census = tnp.index_census(x, BS)
        self.assertTrue(bool(census["inconsistent"].all()))
        img = np.stack([x, x, x]).reshape(3, 1, -1)
        ref = tnp.forward(identity_lut(), img, BS)
        consistent = tnp.forward_consistent_fraction(identity_lut(), img, BS)
        np.testing.assert_allclose(ref - img, 1.0 / 32, atol=2e-6)  # one node too high
        self.assertLess(float(np.abs(consistent - img).max()), 2e-6)

    def test_exact_remainder_equals_reference_except_at_census_inputs(self):
        rng = np.random.default_rng(19)
        lut = random_lut(9)
        codes = rng.integers(0, 256, (3, 64, 512))
        eight_bit = (codes.astype(np.float32) / np.float32(255)).astype(np.float32)
        self.assertTrue(same_bits(tnp.forward_exact_remainder(lut, eight_bit, BS), tnp.forward(lut, eight_bit, BS)))
        img = as_image(edge_values())
        ref, exact = tnp.forward(lut, img, BS), tnp.forward_exact_remainder(lut, img, BS)
        census = np.isin(img.view(np.uint32), np.asarray(CENSUS_BITS, dtype=np.uint32)).any(axis=0)
        differs = (ref.view(np.uint32) != exact.view(np.uint32)).any(axis=0)
        self.assertTrue(bool((differs <= census).all()))  # differences only where an input is a census value
        ideal = tnp.forward_ideal_float64(lut, img)
        self.assertLess(float(np.abs(exact - ideal).max()), 3e-4)  # continuous: no one-node jumps
        self.assertGreater(float(np.abs(ref - ideal).max()), 1e-2)  # the reference does jump there

    def test_no_8bit_or_16bit_code_is_inconsistent(self):
        v8 = np.arange(256, dtype=np.float32) / np.float32(255)
        v16 = np.arange(65536, dtype=np.float32) / np.float32(65535)
        for binsize in (BS, lc.BINSIZE_MODELS_LEGACY(DIM)):
            self.assertFalse(tnp.index_census(v8, binsize)["inconsistent"].any())
            self.assertFalse(tnp.index_census(v16, binsize)["inconsistent"].any())

    def test_domain_errors(self):
        lut = identity_lut()
        for bad in (-1e-6, 1.01, float("nan")):
            img = np.full((3, 1, 1), bad, np.float32)
            with self.assertRaises(tnp.DomainError):
                tnp.forward(lut, img, BS)
            with self.assertRaises(tt.DomainError):
                torch_forward(lut, img)

    def test_tiles_equal_full_frame(self):
        rng = np.random.default_rng(13)
        lut, img = random_lut(6), rng.uniform(0.0, 1.0, (3, 40, 70)).astype(np.float32)
        full = tnp.forward(lut, img, BS)
        tiled = np.empty_like(full)
        for y in range(0, 40, 16):
            for x in range(0, 70, 16):
                tiled[:, y:y + 16, x:x + 16] = torch_forward(lut, np.ascontiguousarray(img[:, y:y + 16, x:x + 16]))
        self.assertTrue(same_bits(tiled, full))

    def test_ideal_float64_close_to_reference(self):
        rng = np.random.default_rng(17)
        lut, img = random_lut(8), rng.uniform(0.0, 1.0, (3, 16, 16)).astype(np.float32)
        self.assertLess(float(np.abs(tnp.forward_ideal_float64(lut, img) - tnp.forward(lut, img, BS)).max()), 3e-4)


class ExtensionApiShim(unittest.TestCase):
    def test_contract(self):
        api = tt.ExtensionApi()
        lut = torch.from_numpy(identity_lut())
        x = torch.rand(1, 3, 4, 5, generator=torch.Generator().manual_seed(1))
        out = torch.empty_like(x)
        self.assertEqual(api.forward(lut, x, out, DIM, DIM ** 3, BS, 4, 5, 1), 1)
        self.assertTrue(torch.equal(out, tt.forward(lut, x, BS)))
        self.assertEqual(api.forward(lut, torch.zeros(1, 1, 2, 2), torch.zeros(1, 1, 2, 2), DIM, DIM ** 3, BS, 2, 2, 1), 0)
        with self.assertRaises(NotImplementedError):
            api.forward(lut, torch.rand(2, 3, 2, 2), torch.empty(2, 3, 2, 2), DIM, DIM ** 3, BS, 2, 2, 2)
        with self.assertRaises(NotImplementedError):
            api.backward()


if __name__ == "__main__":
    unittest.main()
