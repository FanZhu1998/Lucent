"""Trilinear LUT application: parity and numerical validation. Research only.

Blueprint Appendix D2 steps 2 and 5. Three implementations are compared bit for bit:

    extension  trilinear_cpp/src/trilinear.cpp at b491f6d, built unmodified with MSVC
               (build_trilinear_ext.ps1): the repository's CPU code path
    torch      trilinear_torch.forward, pure PyTorch
    numpy      trilinear_numpy.forward, an independent NumPy mirror of the C++ loop

Experiments (results/trilinear_parity.json):

  exhaustive_8bit      every 8-bit RGB colour (256^3) through the a1629 demo LUT, a seeded random LUT
                       with values in [-0.5, 1.5] and the repository's identity LUT; parity, then the
                       identity round trip, the float64 ideal and two candidate fraction variants
  index_census         every float32 in [0, 1] (2**30 + 1 values) for both binsizes: inputs where
                       floor(x / binsize) and fmod(x, binsize) disagree, so the reference
                       interpolates one cell too high; plus the 8-bit and 16-bit code values
  fraction_variants    the candidate production arithmetic (quotient fraction, exact remainder)
                       against the reference on random float32 inputs and at the census inputs
  float_edge_cases     0, subnormals, 1 - ulp, 1, every cell boundary with its float32 neighbours,
                       the values the index census finds, and the largest in-range input above 1
  step5_checks         identity cube, neutral and channel ramps, cube corners, cell midpoints
  tiled_vs_full        the demo frame whole against 256x256 tiles and single rows

Usage (repository root): research\\lut\\.venv\\Scripts\\python.exe research\\lut\\tools\\trilinear_parity.py
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lut_common as lc  # noqa: E402

import numpy as np  # noqa: E402
import torch  # noqa: E402

import trilinear_numpy as tnp  # noqa: E402
import trilinear_torch as tt  # noqa: E402
import upstream_model as um  # noqa: E402

DIM = 33
BS = lc.BINSIZE_MODELS_X(DIM)
BS_LEGACY = lc.BINSIZE_MODELS_LEGACY(DIM)
SEED = 20261009


# ---------------------------------------------------------------------------- implementations


class Implementations:
    def __init__(self, ext) -> None:
        self.ext = ext
        self.names = (["extension"] if ext is not None else []) + ["torch", "numpy"]

    def run(self, name: str, lut: np.ndarray, img: np.ndarray, binsize: float = BS) -> np.ndarray:
        """img: (3, H, W) float32 -> (3, H, W) float32."""
        if name == "numpy":
            return tnp.forward(lut, img, binsize)
        lut_t = torch.from_numpy(np.ascontiguousarray(lut))
        x = torch.from_numpy(np.ascontiguousarray(img)).unsqueeze(0)
        if name == "torch":
            return tt.forward(lut_t, x, binsize)[0].numpy()
        out = torch.empty_like(x)
        rc = self.ext.forward(lut_t, x, out, DIM, DIM ** 3, binsize, x.shape[2], x.shape[3], 1)
        assert rc == 1
        return out[0].numpy()


def bits_equal(a: np.ndarray, b: np.ndarray) -> int:
    """Number of float32 samples whose bit patterns differ (distinguishes -0.0 from 0.0)."""
    return int((a.view(np.uint32) != b.view(np.uint32)).sum())


def ulp_distance(a: np.ndarray, b: np.ndarray) -> int:
    ia = a.view(np.int32).astype(np.int64)
    ib = b.view(np.int32).astype(np.int64)
    ia = np.where(ia < 0, -(ia & 0x7FFFFFFF), ia)
    ib = np.where(ib < 0, -(ib & 0x7FFFFFFF), ib)
    return int(np.abs(ia - ib).max()) if a.size else 0


def quantise(x: np.ndarray) -> np.ndarray:
    """demo_eval: mul(255).add(0.5).clamp(0, 255) then truncation to uint8, in float32."""
    t = torch.from_numpy(np.ascontiguousarray(x)).clone()
    return t.mul_(255).add_(0.5).clamp_(0, 255).to(torch.uint8).numpy()


def eight_bit_values() -> np.ndarray:
    """torchvision to_tensor: uint8 -> float32, then div(255) in float32."""
    return torch.arange(256, dtype=torch.uint8).to(torch.float32).div(255).numpy()


# ---------------------------------------------------------------------------- LUTs


def identity_lut_from_text(checkout: Path) -> tuple[np.ndarray, dict[str, Any]]:
    """Parses IdentityLUT33.txt exactly as Generator3DLUT_identity does: line n = i*D*D + j*D + k
    gives buffer[c, i, j, k] = float(column c)."""
    lines = (checkout / "IdentityLUT33.txt").read_text().splitlines()
    buf = np.zeros((3, DIM, DIM, DIM), dtype=np.float32)
    for i in range(DIM):
        for j in range(DIM):
            for k in range(DIM):
                x = lines[i * DIM * DIM + j * DIM + k].split()
                buf[0, i, j, k], buf[1, i, j, k], buf[2, i, j, k] = float(x[0]), float(x[1]), float(x[2])
    g = (np.arange(DIM, dtype=np.float64) / (DIM - 1)).astype(np.float32)
    exact = np.stack([np.broadcast_to(g[None, None, :], (DIM,) * 3), np.broadcast_to(g[None, :, None], (DIM,) * 3),
                      np.broadcast_to(g[:, None, None], (DIM,) * 3)]).astype(np.float32)
    return buf, {"lines": len(lines), "equals_exact_k_over_32_grid": bool(np.array_equal(buf, exact)),
                 "layout_check": "buffer[0] varies along the last (r) axis, buffer[2] along the first (b) axis"}


def luts(checkout: Path) -> tuple[dict[str, np.ndarray], dict[str, Any]]:
    model = um.load(checkout)
    img = um.load_image(checkout / "demo_images" / "sRGB" / "a1629.jpg")
    _, demo = um.fuse(model, img)
    ident, ident_info = identity_lut_from_text(checkout)
    rng = np.random.default_rng(SEED)
    out = {
        "a1629_demo": demo.numpy().astype(np.float32),
        "random_uniform_-0.5_1.5": rng.uniform(-0.5, 1.5, (3, DIM, DIM, DIM)).astype(np.float32),
        "identity_txt": ident,
    }
    info: dict[str, Any] = {name: lc.array_record(v) for name, v in out.items()}
    info["identity_txt_details"] = ident_info
    info["random_seed"] = SEED
    recorded = lc.RESULTS_DIR / "demo_reproduction.json"
    if recorded.exists():
        rec = json.loads(recorded.read_text(encoding="utf-8"))["images"]["a1629.jpg"]["demo"]["fused_lut"]["sha256"]
        info["a1629_demo_matches_demo_reproduction_json"] = rec == info["a1629_demo"]["sha256"]
    return out, info


# ---------------------------------------------------------------------------- experiments


def exhaustive_8bit(impl: Implementations, lut_set: dict[str, np.ndarray]) -> dict[str, Any]:
    v = eight_bit_values()
    rows_per_chunk = 256  # 256 x 4096 pixels = 2**20 colours per chunk
    n_chunks = 16
    result: dict[str, Any] = {"colours": 256 ** 3, "layout": "pixel n -> (r, g, b) = (n % 256, n // 256 % 256, n // 65536)"}
    for lut_name, lut in lut_set.items():
        mism = {name: 0 for name in impl.names if name != "numpy"}
        max_ulp = {name: 0 for name in mism}
        seconds = {name: 0.0 for name in impl.names}
        ideal_err = 0.0
        cons_float, cons_u8, cons_max = 0, 0, 0.0
        exact_float = 0
        ident_u8_mismatch, ident_max_err = 0, 0.0
        for c in range(n_chunks):
            n = np.arange(c * rows_per_chunk * 4096, (c + 1) * rows_per_chunk * 4096, dtype=np.int64)
            codes = np.stack([n % 256, (n // 256) % 256, n // 65536]).astype(np.int64)
            img = v[codes].reshape(3, rows_per_chunk, 4096).astype(np.float32)
            outs = {}
            for name in impl.names:
                t0 = time.perf_counter()
                outs[name] = impl.run(name, lut, img)
                seconds[name] += time.perf_counter() - t0
            ref = outs["numpy"]
            for name in mism:
                d = bits_equal(outs[name], ref)
                mism[name] += d
                if d:
                    max_ulp[name] = max(max_ulp[name], ulp_distance(outs[name], ref))
            ideal_err = max(ideal_err, float(np.abs(tnp.forward_ideal_float64(lut, img) - ref).max()))
            cons = tnp.forward_consistent_fraction(lut, img, BS)
            cons_float += bits_equal(cons, ref)
            cons_u8 += int((quantise(cons) != quantise(ref)).sum())
            cons_max = max(cons_max, float(np.abs(cons - ref).max()))
            exact_float += bits_equal(tnp.forward_exact_remainder(lut, img, BS), ref)
            if lut_name == "identity_txt":
                ident_u8_mismatch += int((quantise(ref) != codes.reshape(3, rows_per_chunk, 4096).astype(np.uint8)).sum())
                ident_max_err = max(ident_max_err, float(np.abs(ref - img).max()))
        entry: dict[str, Any] = {
            "samples": 3 * 256 ** 3,
            "bit_mismatches_vs_numpy": mism,
            "max_ulp_vs_numpy": max_ulp,
            "max_abs_difference_vs_float64_ideal": ideal_err,
            "quotient_fraction_variant": {
                "float_samples_differing": cons_float, "max_abs_difference": cons_max,
                "uint8_samples_differing": cons_u8,
            },
            "exact_remainder_variant": {"float_samples_differing": exact_float},
            "seconds": {k: round(s, 3) for k, s in seconds.items()},
            "megapixels_per_second": {k: round(256 ** 3 / s / 1e6, 2) for k, s in seconds.items()},
        }
        if lut_name == "identity_txt":
            entry["identity_round_trip"] = {
                "uint8_samples_changed": ident_u8_mismatch,
                "max_abs_float_error": ident_max_err,
                "note": "with binsize = 1.000001/32 the identity cube maps x to about x / 1.000001; the "
                        "error stays far below half a code value, so the 8-bit round trip is exact",
            }
        result[lut_name] = entry
    return result


def find_last_in_range(binsize: float) -> np.float32:
    """Largest float32 x with floor(fl32(x / binsize)) <= DIM - 2 (the top cell)."""
    bs = np.float32(binsize)
    lo, hi = np.uint32(0x3F800000), np.uint32(0x3F810000)  # [1.0, 1.0078125]
    assert np.floor(hi.view(np.float32) / bs) > DIM - 2
    while hi - lo > 1:
        mid = np.uint32((int(lo) + int(hi)) // 2)
        if np.floor(mid.view(np.float32) / bs) <= DIM - 2:
            lo = mid
        else:
            hi = mid
    return lo.view(np.float32)


def index_census(binsize: float, label: str) -> dict[str, Any]:
    bs = np.float32(binsize)
    stop = 0x3F800000 + 1  # through 1.0
    chunk = 1 << 24
    examples: list[dict[str, Any]] = []
    count = 0
    frac_min = 1.0
    for start in range(0, stop, chunk):
        bits = np.arange(start, min(stop, start + chunk), dtype=np.uint32)
        c = tnp.index_census(bits.view(np.float32), binsize)
        m = c["inconsistent"]
        if m.any():
            assert (c["k_ref"][m] == c["k_exact"][m] + 1).all()
            count += int(m.sum())
            frac_min = min(frac_min, float(c["frac_ref"][m].min()))
            for b, kr, ke, fr in zip(bits[m], c["k_ref"][m], c["k_exact"][m], c["frac_ref"][m]):
                examples.append({"x": float(np.uint32(b).view(np.float32)), "bits": "0x%08x" % int(b),
                                 "cell_reference": int(kr), "cell_exact": int(ke), "fraction_reference": float(fr)})
    v8 = eight_bit_values()
    v16 = torch.arange(65536, dtype=torch.int32).to(torch.float32).div(65535).numpy()
    return {
        "binsize": binsize, "binsize_float32_bits": lc.float32_bits(float(bs)), "label": label,
        "float32_values_tested": stop,
        "inconsistent_values": count,
        "inconsistent_fraction_reference_min": frac_min if count else None,
        "inconsistent_8bit_codes": int(tnp.index_census(v8, binsize)["inconsistent"].sum()),
        "inconsistent_16bit_codes": int(tnp.index_census(v16, binsize)["inconsistent"].sum()),
        "largest_in_range_input": float(find_last_in_range(binsize)),
        "examples": examples,
    }


def fraction_variants(lut_set: dict[str, np.ndarray], census: dict[str, Any]) -> dict[str, Any]:
    """Candidate production arithmetic against the reference on inputs that are NOT 8-bit codes:
    4,194,304 seeded uniform float32 pixels per LUT, and the census inputs (where the reference
    jumps one node). Variants: 'quotient' (q - floor(q)) and 'exact_remainder' (double remainder,
    clamped)."""
    rng = np.random.default_rng(SEED + 1)
    img = rng.uniform(0.0, 1.0, (3, 1024, 4096)).astype(np.float32)
    x = np.asarray([e["x"] for e in census["examples"]], dtype=np.float32)
    jumps = np.stack([x, x, x]).reshape(3, 1, -1)
    out: dict[str, Any] = {"random_float32_pixels": int(img.shape[1] * img.shape[2]),
                           "census_inputs": int(x.size)}
    for name, lut in lut_set.items():
        ref = tnp.forward(lut, img, BS)
        exact = tnp.forward_exact_remainder(lut, img, BS)
        quot = tnp.forward_consistent_fraction(lut, img, BS)
        j_ref = tnp.forward(lut, jumps, BS)
        j_exact = tnp.forward_exact_remainder(lut, jumps, BS)
        j_ideal = tnp.forward_ideal_float64(lut, jumps)
        out[name] = {
            "exact_remainder_float_samples_differing": bits_equal(exact, ref),
            "quotient_float_samples_differing": bits_equal(quot, ref),
            "quotient_max_abs_difference": float(np.abs(quot - ref).max()),
            "census_reference_max_abs_error_vs_float64_ideal": float(np.abs(j_ref - j_ideal).max()),
            "census_exact_remainder_max_abs_error_vs_float64_ideal": float(np.abs(j_exact - j_ideal).max()),
        }
    return out


def float_edge_cases(impl: Implementations, lut_set: dict[str, np.ndarray], census: dict[str, Any]) -> dict[str, Any]:
    bs = np.float32(BS)
    vals: list[np.float32] = [np.float32(0), np.float32(1e-45), np.float32(1e-30), np.float32(1.0 / 255),
                              np.float32(0.5), np.nextafter(np.float32(1), np.float32(0)), np.float32(1),
                              find_last_in_range(BS)]
    for k in range(1, DIM - 1):
        x0 = np.float32(np.float64(k) * np.float64(bs))
        vals += [x0, np.nextafter(x0, np.float32(0)), np.nextafter(x0, np.float32(2)),
                 np.nextafter(np.nextafter(x0, np.float32(0)), np.float32(0)),
                 np.float32((2 * k + 1) / 64.0)]
    vals += [np.float32(e["x"]) for e in census["examples"]]
    vals = sorted({float(x) for x in vals})
    arr = np.asarray(vals, dtype=np.float32)
    n = arr.size
    # every value appears in every channel, against varied partners
    img = np.stack([arr, np.roll(arr, n // 3), arr[::-1]]).reshape(3, 1, n).astype(np.float32)
    out: dict[str, Any] = {"values": n}
    for lut_name, lut in lut_set.items():
        outs = {name: impl.run(name, lut, img) for name in impl.names}
        out[lut_name] = {name: bits_equal(outs[name], outs["numpy"]) for name in impl.names if name != "numpy"}
    return out


def step5_checks(impl: Implementations, lut_set: dict[str, np.ndarray]) -> dict[str, Any]:
    v = eight_bit_values()
    z = np.zeros(256, np.float32)
    ramps = {"neutral": (v, v, v), "red": (v, z, z), "green": (z, v, z), "blue": (z, z, v)}
    name0 = impl.names[0]  # the extension when built
    result: dict[str, Any] = {"implementation": name0}
    ident = lut_set["identity_txt"]
    for rname, (r, g, b) in ramps.items():
        img = np.stack([r, g, b]).reshape(3, 1, 256)
        o_id = impl.run(name0, ident, img)
        o_demo = impl.run(name0, lut_set["a1629_demo"], img)
        q_demo = quantise(o_demo).astype(np.int16)
        demo = []
        for c in range(3):
            steps = np.diff(o_demo[c, 0])
            down = np.where(steps < 0)[0]
            demo.append({
                "float_decreasing_steps": int(down.size),
                "most_negative_step": float(steps.min()),
                "lowest_output_at_a_decrease": float(o_demo[c, 0, down].min()) if down.size else None,
                "uint8_decreasing_steps": int((np.diff(q_demo[c, 0]) < 0).sum()),
            })
        result[f"ramp_{rname}"] = {
            "identity_max_abs_error": float(np.abs(o_id - img).max()),
            "identity_uint8_round_trip_exact": bool(np.array_equal(quantise(o_id), (img * 255 + 0.5).astype(np.uint8))),
            "a1629_demo_lut_per_output_channel": demo,
        }
    corners = np.array([[(i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1] for i in range(8)], np.float32).T.reshape(3, 1, 8)
    result["cube_corners"] = {}
    for lut_name, lut in lut_set.items():
        o = impl.run(name0, lut, corners)
        idx = corners.reshape(3, 8).astype(int) * (DIM - 1)
        expect = np.stack([lut[c, idx[2], idx[1], idx[0]] for c in range(3)]).reshape(3, 1, 8)
        result["cube_corners"][lut_name] = {
            "max_abs_difference_from_lut_corner": float(np.abs(o - expect).max()),
            "output_at_white_for_identity": [float(x) for x in o[:, 0, 7]] if lut_name == "identity_txt" else None,
        }
    # cell midpoints along each axis, the other two channels on grid nodes
    mids = ((2 * np.arange(DIM - 1) + 1) / 64.0).astype(np.float32)
    nodes = (np.arange(DIM - 1) / 32.0).astype(np.float32)
    result["cell_midpoints"] = {}
    for lut_name, lut in lut_set.items():
        worst = 0.0
        for axis in range(3):
            chans = [np.repeat(nodes, DIM - 1), np.tile(nodes, DIM - 1)]
            m = np.tile(mids, DIM - 1)
            planes = chans[:]
            planes.insert(axis, m)
            img = np.stack(planes).reshape(3, 1, -1).astype(np.float32)
            o = impl.run(name0, lut, img)
            worst = max(worst, float(np.abs(o - tnp.forward_ideal_float64(lut, img)).max()))
        result["cell_midpoints"][lut_name] = {"max_abs_difference_vs_float64_ideal": worst}
    return result


def tiled_vs_full(impl: Implementations, lut: np.ndarray, checkout: Path) -> dict[str, Any]:
    img = um.load_image(checkout / "demo_images" / "sRGB" / "a1629.jpg")[0].numpy()
    result: dict[str, Any] = {"image": "a1629.jpg", "shape": list(img.shape)}
    for name in impl.names:
        full = impl.run(name, lut, img)
        _, h, w = img.shape
        tiled = np.empty_like(full)
        for y in range(0, h, 256):
            for x in range(0, w, 256):
                tiled[:, y:y + 256, x:x + 256] = impl.run(name, lut, np.ascontiguousarray(img[:, y:y + 256, x:x + 256]))
        rows = np.empty_like(full)
        if name != "numpy":  # one row at a time is slow in NumPy and adds nothing
            for y in range(h):
                rows[:, y:y + 1, :] = impl.run(name, lut, np.ascontiguousarray(img[:, y:y + 1, :]))
        result[name] = {
            "tiles_256_bit_mismatches": bits_equal(tiled, full),
            "single_rows_bit_mismatches": bits_equal(rows, full) if name != "numpy" else None,
        }
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=lc.RESULTS_DIR / "trilinear_parity.json")
    args = ap.parse_args()
    checkout = lc.checkout_path()
    upstream = lc.verify_checkout(checkout)
    ext = lc.load_extension()
    impl = Implementations(ext)
    lut_set, lut_info = luts(checkout)
    t0 = time.perf_counter()
    census = {"models_x": index_census(BS, "models_x.py, 1.000001 / 32 (reproduced path)"),
              "models_legacy": index_census(BS_LEGACY, "models.py, 1.0001 / 32 (PyTorch 0.4.1 path)")}
    print(f"census done ({time.perf_counter() - t0:.0f} s): {census['models_x']['inconsistent_values']} / "
          f"{census['models_legacy']['inconsistent_values']} inconsistent float32 inputs")
    report: dict[str, Any] = {
        "schema": "lucent.lut-research.trilinear-parity",
        "schema_version": 1,
        "run_date": dt.date.today().isoformat(),
        "upstream": upstream,
        "environment": lc.environment(),
        "extension_build": lc.extension_build_record(ext),
        "implementations": impl.names,
        "binsize": {"value": BS, "float32_bits": lc.float32_bits(BS)},
        "luts": lut_info,
        "exhaustive_8bit": exhaustive_8bit(impl, lut_set),
        "index_census": census,
        "fraction_variants": fraction_variants(lut_set, census["models_x"]),
        "float_edge_cases": float_edge_cases(impl, lut_set, census["models_x"]),
        "step5_checks": step5_checks(impl, lut_set),
        "tiled_vs_full": tiled_vs_full(impl, lut_set["a1629_demo"], checkout),
    }
    total = sum(sum(e["bit_mismatches_vs_numpy"].values()) for k, e in report["exhaustive_8bit"].items()
                if isinstance(e, dict) and "bit_mismatches_vs_numpy" in e)
    total += sum(sum(v.values()) for k, v in report["float_edge_cases"].items() if isinstance(v, dict))
    tiles = report["tiled_vs_full"]
    total += sum(v["tiles_256_bit_mismatches"] + (v["single_rows_bit_mismatches"] or 0)
                 for k, v in tiles.items() if isinstance(v, dict))
    report["all_parity_checks_bit_exact"] = total == 0
    lc.assert_checkout_unchanged(checkout)
    lc.write_json(args.out, report)
    print(f"wrote {args.out}; implementations {impl.names}; bit-exact: {total == 0}")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
