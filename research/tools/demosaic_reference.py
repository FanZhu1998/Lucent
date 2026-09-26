#!/usr/bin/env python3
"""Malvar-He-Cutler reference crops for Lucent's native Bayer route (blueprint Appendix A.3).

For each crop listed in ``CROPS`` this tool

1. decodes the cached camera fixture with rawpy (LibRaw) after checking its SHA-256 against
   ``fixtures/manifest.json``;
2. normalises the visible area exactly as Lucent's ``engine::normalize_sensor`` does for data
   without a spatial black pattern or black deltas::

       value = float32((float64(raw) - black[c]) / (white - black[c]))

   where ``c`` is the LibRaw colour index of the site (``raw_colors_visible``), ``black`` is
   ``black_level_per_channel`` and ``white`` is ``white_level``; nothing is clipped;
3. cuts a ``SIZE`` x ``SIZE`` crop whose origin has the listed parity (so the crops cover all
   four Bayer phases) at the most textured unsaturated position of a fixed grid of candidates;
4. demosaics the float32 crop with colour-demosaicing's ``demosaicing_CFA_Bayer_Malvar2004``
   (BSD-3-Clause; it computes in float64) using the CFA pattern at the crop origin;
5. writes ``fixtures/reference/demosaic/<fixture id>.json`` (schema ``lucent.demosaic-reference``
   version 1) with the crop, the result (little-endian float32, base64, with SHA-256), the exact
   normalisation inputs and the tool versions.

The reference convolves with ``scipy.ndimage.convolve(mode="reflect")``, which mirrors across the
crop edge without keeping the Bayer phase, so only pixels at least ``INTERIOR_MARGIN`` (the
kernel radius, 2) from the crop edge are comparable with Lucent's phase-preserving borders.

Usage::

    demosaic_reference.py [--manifest fixtures/manifest.json] [--out-dir fixtures/reference/demosaic]
    demosaic_reference.py --check      # regenerate in memory and compare with the committed files

Exit status: 0 success (or --check: every committed file reproduced exactly), 1 a fixture is
missing or fails its hash, or --check found a difference, 2 usage errors.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import sys
import warnings
from pathlib import Path
from typing import Any, Sequence

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "fixtures" / "manifest.json"
DEFAULT_OUT_DIR = REPO_ROOT / "fixtures" / "reference" / "demosaic"

SCHEMA = "lucent.demosaic-reference"
SCHEMA_VERSION = 1
SIZE = 64
INTERIOR_MARGIN = 2
GRID = 12  # candidate positions per axis

# (fixture id, crop-origin parity (x % 2, y % 2) in visible-area coordinates). Every fixture has
# RGGB at its visible origin, so the four parities give the four Bayer phases.
CROPS: list[tuple[str, tuple[int, int]]] = [
    ("canon-eos-r5-cr3-raw", (0, 0)),
    ("nikon-z-6-nef-lossless", (1, 0)),
    ("sony-ilce-7m4-arw-lossless-l", (0, 1)),
    ("panasonic-dc-s5m2-rw2", (1, 1)),
]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def float32_block(array: np.ndarray) -> dict[str, Any]:
    """Little-endian float32 samples, row-major, base64 with their SHA-256."""
    data = np.ascontiguousarray(array, dtype="<f4").tobytes()
    return {
        "dtype": "float32-le",
        "shape": list(array.shape),
        "sha256": sha256_bytes(data),
        "base64": base64.b64encode(data).decode("ascii"),
    }


def versions() -> dict[str, str]:
    import rawpy
    import scipy

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # colour-science warns that matplotlib is not installed
        import colour
        import colour_demosaicing

    return {
        "tool": "research/tools/demosaic_reference.py",
        "rawpy": rawpy.__version__,
        "libraw": ".".join(str(part) for part in rawpy.libraw_version),
        "colour_demosaicing": colour_demosaicing.__version__,
        "colour_science": colour.__version__,
        "numpy": np.__version__,
        "scipy": scipy.__version__,
    }


def normalise(raw: Any) -> tuple[np.ndarray, np.ndarray, list[float], float, np.ndarray]:
    """Normalised visible area (float32), channel index per site, black per channel, white,
    and the saturation threshold per site (stored units)."""
    visible = np.asarray(raw.raw_image_visible)
    channels = np.asarray(raw.raw_colors_visible)
    black = [float(value) for value in raw.black_level_per_channel]
    white = float(raw.white_level)
    black_site = np.asarray(black, dtype=np.float64)[channels]
    values = (visible.astype(np.float64) - black_site) / (white - black_site)
    limits = raw.camera_white_level_per_channel
    threshold = np.full(4, white)
    if limits is not None:
        for c, limit in enumerate(limits):
            if black[c] < limit < white:
                threshold[c] = float(limit)
    return values.astype(np.float32), channels, black, white, threshold[channels]


def choose_origin(values: np.ndarray, saturated: np.ndarray, parity: tuple[int, int]) -> tuple[int, int]:
    """The most textured (highest standard deviation) unsaturated crop of a fixed grid."""
    height, width = values.shape
    best: tuple[float, int, int] | None = None
    for gy in range(GRID):
        for gx in range(GRID):
            x = (width - SIZE) * (gx + 1) // (GRID + 1)
            y = (height - SIZE) * (gy + 1) // (GRID + 1)
            x -= (x - parity[0]) % 2
            y -= (y - parity[1]) % 2
            window = (slice(y, y + SIZE), slice(x, x + SIZE))
            if saturated[window].any():
                continue
            score = float(values[window].std(dtype=np.float64))
            if best is None or score > best[0]:
                best = (score, x, y)
    if best is None:
        raise RuntimeError("every candidate crop contains saturated samples")
    return best[1], best[2]


def pattern_at(channels: np.ndarray, color_desc: bytes, x: int, y: int) -> str:
    letters = color_desc.decode("ascii")
    return "".join(letters[int(channels[y + dy, x + dx])] for dy in (0, 1) for dx in (0, 1))


def build_reference(
    fixture_id: str, parity: tuple[int, int], manifest: dict[str, Any], cache_dir: Path
) -> dict[str, Any]:
    import rawpy

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        from colour_demosaicing import demosaicing_CFA_Bayer_Malvar2004

    entry = next((e for e in manifest["fixtures"] if e["id"] == fixture_id), None)
    if entry is None:
        raise LookupError(f"{fixture_id} is not in the manifest")
    path = cache_dir / fixture_id / entry["file_name"]
    if not path.is_file():
        raise FileNotFoundError(f"{path} is missing; run tools\\fetch-fixtures.ps1 -Tier all")
    if sha256_file(path) != entry["sha256"]:
        raise ValueError(f"{path} does not match its manifest SHA-256")

    with rawpy.imread(str(path)) as raw:
        if raw.raw_type != rawpy.RawType.Flat or np.asarray(raw.raw_pattern).shape != (2, 2):
            raise ValueError(f"{fixture_id} is not a 2 x 2 Bayer mosaic")
        color_desc = bytes(raw.color_desc)
        values, channels, black, white, threshold = normalise(raw)
        visible = np.asarray(raw.raw_image_visible)
        saturated = visible >= threshold
        x, y = choose_origin(values, saturated, parity)
        crop = np.ascontiguousarray(values[y : y + SIZE, x : x + SIZE])
        pattern = pattern_at(channels, color_desc, x, y)
        if sorted(pattern) != sorted("RGGB"):
            raise ValueError(f"{fixture_id}: pattern {pattern} at ({x}, {y}) is not Bayer")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            rgb = demosaicing_CFA_Bayer_Malvar2004(crop, pattern)
        rgb32 = np.asarray(rgb, dtype=np.float32)
        return {
            "schema": SCHEMA,
            "schema_version": SCHEMA_VERSION,
            "generator": versions(),
            "fixture": {"id": fixture_id, "file_name": entry["file_name"], "sha256": entry["sha256"]},
            "crop": {"frame": "visible_area", "x": x, "y": y, "width": SIZE, "height": SIZE},
            "cfa_pattern": pattern,
            "normalization": {
                "formula": "float32((float64(raw) - black[c]) / (white - black[c])), no clipping",
                "channel_of_site": "LibRaw colour index (rawpy raw_colors_visible)",
                "color_desc": color_desc.decode("ascii"),
                "black_per_channel": black,
                "white": white,
                "crop_raw_min": int(visible[y : y + SIZE, x : x + SIZE].min()),
                "crop_raw_max": int(visible[y : y + SIZE, x : x + SIZE].max()),
            },
            "algorithm": {
                "function": "colour_demosaicing.demosaicing_CFA_Bayer_Malvar2004",
                "licence": "BSD-3-Clause",
                "precision": "float64, stored as float32",
                "border": "scipy.ndimage.convolve mode='reflect' (does not keep the Bayer phase)",
            },
            "interior_margin": INTERIOR_MARGIN,
            "mosaic": float32_block(crop),
            "rgb": float32_block(rgb32),
        }


def dumps(document: dict[str, Any]) -> str:
    return json.dumps(document, indent=1, ensure_ascii=True) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--check", action="store_true", help="compare with the committed files instead of writing")
    args = parser.parse_args(argv)
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"cannot read the manifest: {error}", file=sys.stderr)
        return 2

    status = 0
    for fixture_id, parity in CROPS:
        try:
            text = dumps(build_reference(fixture_id, parity, manifest, args.manifest.parent / "cache"))
        except (LookupError, OSError, ValueError, RuntimeError) as error:
            print(f"{fixture_id}: {error}", file=sys.stderr)
            status = 1
            continue
        target = args.out_dir / f"{fixture_id}.json"
        if args.check:
            committed = target.read_text(encoding="utf-8") if target.is_file() else None
            same = committed == text
            print(f"{fixture_id}: {'reproduced' if same else 'DIFFERS'}")
            status = status if same else 1
        else:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8", newline="\n")
            print(f"{fixture_id}: wrote {target} ({len(text)} bytes)")
    return status


if __name__ == "__main__":
    sys.exit(main())
