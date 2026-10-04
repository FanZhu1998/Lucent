#!/usr/bin/env python3
"""Guided-filter reference cases for Lucent's native guided filter (blueprint Appendix A.2).

For each case in ``CASES`` this tool

1. builds a deterministic float32 guide and input (numpy ``default_rng`` with a fixed seed;
   steps, ramps, textures and noise in the value ranges the local-tone operation uses, plus a
   window larger than the image and a multi-channel input);
2. filters them with OpenCV's ``cv2.ximgproc.guidedFilter(guide, input, radius, eps)`` from
   ``opencv-contrib-python`` (float32 path, default ``scale`` 1, i.e. the exact, not the fast,
   guided filter): the selected reference implementation;
3. evaluates the same formula exactly (float64, numpy) with OpenCV's conventions, which this tool
   first verifies against OpenCV::

       f = normalised (2r+1) x (2r+1) box mean, BORDER_REFLECT ("fedcba|abcdef")
       a = (f(I p) - f(I) f(p)) / (f(I I) - f(I)^2 + eps),  b = f(p) - a f(I)
       q = f(a) I + f(b)

   OpenCV's float32 arithmetic deviates from the exact value; the largest deviation per case is
   stored (``opencv_vs_exact_max``) so the C++ test can bound OpenCV parity by it;
4. writes ``src/engine/tests/data/guided_filter/<case>.json`` (schema
   ``lucent.guided-filter-reference`` version 1): sizes, radius, eps, the guide, the input (or
   ``null`` when the input is the guide), OpenCV's result and the exact result, each as
   little-endian float32 base64 with its SHA-256, plus the tool versions.

The OpenCV package is a developer-only reference: install it into a separate virtual
environment (never the junctioned research/.venv)::

    python -m venv build/w6-eng/gf-venv
    build/w6-eng/gf-venv/Scripts/python -m pip install numpy==2.5.3 opencv-contrib-python-headless
    build/w6-eng/gf-venv/Scripts/python src/engine/tests/tools/guided_filter_reference.py [--check]

Exit status: 0 success (or --check: every committed file reproduced exactly), 1 a difference or
a failed self-check, 2 usage errors.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Callable

import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover - environment problem, reported clearly
    print("error: OpenCV (opencv-contrib-python) is not installed in this interpreter", file=sys.stderr)
    sys.exit(2)

REPO_ROOT = Path(__file__).resolve().parents[4]
DEFAULT_OUT_DIR = REPO_ROOT / "src" / "engine" / "tests" / "data" / "guided_filter"
SCHEMA = "lucent.guided-filter-reference"
SCHEMA_VERSION = 1
# The exact formula must reproduce OpenCV to within this (absolute, relative to max(1, |value|)):
# OpenCV evaluates in float32; anything larger means the conventions differ.
SELF_CHECK_TOLERANCE = 5e-4


def box(a: np.ndarray, r: int) -> np.ndarray:
    """Normalised box mean with BORDER_REFLECT, exact in float64 (summed-area table)."""
    a = a.astype(np.float64)
    pad = [(r, r), (r, r)] + [(0, 0)] * (a.ndim - 2)
    p = np.pad(a, pad, mode="symmetric")  # numpy "symmetric" == OpenCV BORDER_REFLECT
    pad_c = [(1, 0), (1, 0)] + [(0, 0)] * (a.ndim - 2)
    c = np.cumsum(np.cumsum(np.pad(p, pad_c), axis=0), axis=1)
    k = 2 * r + 1
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / float(k * k)


def exact_guided_filter(guide: np.ndarray, src: np.ndarray, r: int, eps: float) -> np.ndarray:
    g = guide.astype(np.float64)
    p = src.astype(np.float64)
    if p.ndim == 3:
        gi = g[:, :, None]
    else:
        gi = g
    mean_i = box(g, r)
    mean_ii = box(g * g, r)
    var = mean_ii - mean_i * mean_i
    mean_p = box(p, r)
    mean_ip = box(gi * p, r)
    if p.ndim == 3:
        mi = mean_i[:, :, None]
        var = var[:, :, None]
    else:
        mi = mean_i
    a = (mean_ip - mi * mean_p) / (var + eps)
    b = mean_p - a * mi
    return box(a, r) * gi + box(b, r)


def make_step_noise(rng: np.random.Generator) -> tuple[np.ndarray, None]:
    h, w = 48, 64
    x = np.arange(w)[None, :].repeat(h, axis=0)
    guide = np.where(x < 29, -6.0, -1.0) + rng.normal(0.0, 0.1, (h, w))
    return guide.astype(np.float32), None


def make_ramp_texture(rng: np.random.Generator) -> tuple[np.ndarray, None]:
    h, w = 48, 64
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    guide = -10.0 + 10.0 * x / (w - 1) + 0.3 * np.sin(x * 0.9) * np.cos(y * 0.7) + rng.normal(0.0, 0.05, (h, w))
    return guide.astype(np.float32), None


def make_two_image(rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray]:
    h, w = 40, 52
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    guide = np.where((x - 26) ** 2 + (y - 20) ** 2 < 150, 0.8, 0.2) + 0.05 * rng.normal(0.0, 1.0, (h, w))
    src = np.stack([guide + 0.2 * rng.normal(0.0, 1.0, (h, w)), 0.5 * x / w + 0.1 * rng.normal(0.0, 1.0, (h, w))],
                   axis=2)
    return guide.astype(np.float32), src.astype(np.float32)


def make_large_radius(rng: np.random.Generator) -> tuple[np.ndarray, None]:
    return (rng.random((17, 23)) * 4.0 - 2.0).astype(np.float32), None


def make_unit_range(rng: np.random.Generator) -> tuple[np.ndarray, None]:
    return rng.random((31, 37)).astype(np.float32), None


# name -> (builder, radius, eps, seed)
CASES: dict[str, tuple[Callable[[np.random.Generator], tuple[np.ndarray, Any]], int, float, int]] = {
    "step_noise": (make_step_noise, 4, 0.0625, 101),
    "ramp_texture": (make_ramp_texture, 6, 0.0625, 202),
    "two_image": (make_two_image, 3, 0.01, 303),
    "large_radius": (make_large_radius, 20, 0.5, 404),
    "unit_range": (make_unit_range, 1, 0.001, 505),
}


def encode(a: np.ndarray) -> dict[str, str]:
    data = np.ascontiguousarray(a, dtype="<f4").tobytes()
    return {"base64": base64.b64encode(data).decode("ascii"), "sha256": hashlib.sha256(data).hexdigest()}


def build_case(name: str) -> dict[str, Any]:
    builder, radius, eps, seed = CASES[name]
    rng = np.random.default_rng(seed)
    guide, src = builder(rng)
    filtered_input = guide if src is None else src
    opencv = cv2.ximgproc.guidedFilter(guide, filtered_input, radius, eps)
    opencv = np.asarray(opencv, dtype=np.float32).reshape(filtered_input.shape)
    exact = exact_guided_filter(guide, filtered_input, radius, eps)
    scale = max(1.0, float(np.abs(exact).max()))
    deviation = float(np.abs(opencv.astype(np.float64) - exact).max())
    if deviation > SELF_CHECK_TOLERANCE * scale:
        raise SystemExit(f"error: case {name}: the exact formula deviates from OpenCV by {deviation:.3g}; "
                         "the conventions do not match")
    channels = 1 if filtered_input.ndim == 2 else filtered_input.shape[2]
    return {
        "schema": SCHEMA,
        "version": SCHEMA_VERSION,
        "case": name,
        "width": int(guide.shape[1]),
        "height": int(guide.shape[0]),
        "input_channels": int(channels),
        "radius": radius,
        "eps": eps,
        "seed": seed,
        "guide": encode(guide),
        "input": None if src is None else encode(src),
        "opencv": encode(opencv),
        "exact": encode(exact.astype(np.float32)),
        "opencv_vs_exact_max": deviation,
        "tools": {"opencv": cv2.__version__, "numpy": np.__version__, "python": sys.version.split()[0]},
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--check", action="store_true", help="compare with the committed files instead of writing")
    args = parser.parse_args(argv)
    status = 0
    for name in CASES:
        case = build_case(name)
        text = json.dumps(case, indent=1, sort_keys=True) + "\n"
        path = args.out_dir / f"{name}.json"
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                print(f"difference: {path}")
                status = 1
        else:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {path} (OpenCV vs exact: {case['opencv_vs_exact_max']:.3g})")
    return status


if __name__ == "__main__":
    sys.exit(main())
