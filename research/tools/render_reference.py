#!/usr/bin/env python3
"""Compare Lucent's render path with a rawpy/LibRaw rendering of the same fixtures (ADR 0006).

For each selected fixture of ``fixtures/manifest.json`` this tool

1. checks the cached file's SHA-256 against the manifest;
2. runs ``lucent-render <file> --out <work>/<id>/export --subimage 0 --legacy-neutral-v0
   --dump-linear <linear.tiff> --report <report.json>``: the W2 render path (ADR 0006; the develop
   pipeline of ADR 0010 has no scene-linear dump, and its look is compared by the C++ tests), whose
   dump is Lucent's scene-linear image before display rendering, upright, converted to linear sRGB
   primaries (D65), 32-bit float;
3. renders the file with rawpy ``postprocess`` using as-shot white balance, no automatic
   brightening, linear output (``gamma=(1, 1)``), 16 bits, sRGB primaries, highlight clipping,
   ``adjust_maximum_thr=0`` (LibRaw's image-dependent lowering of the white level is disabled
   because Lucent does not do it), LibRaw's default AHD demosaic (X-Trans: its own route) and NO
   ``user_flip``: LibRaw applies the file's own orientation, so the geometry of the two images,
   and the reference orientation recorded (``sizes.flip`` through dcraw's EXIF table
   ``"50132467"``), are independent of Lucent. A size or orientation mismatch fails the fixture;
4. clips Lucent's values per channel to [0, 1] (rawpy's 16-bit output is clipped there),
   box-downscales both images by ``--downscale`` (default 8) and computes, per cell, CIEDE2000
   between them (sRGB -> XYZ from the primaries, CIELAB relative to the D65 white with Y = 1),
   reporting the mean, 95th percentile and maximum, and the mean luminance ratio
   ``mean(Y_lucent) / mean(Y_rawpy)``;
5. also computes the region statistics that the C++ integration tests gate on: the mean linear
   sRGB of each cell of a ``--grid`` x ``--grid`` partition (rows ``[floor(i H / G), floor((i + 1) H / G))``,
   columns likewise) and CIEDE2000 between the region means.

Exposure conventions agree without any correction: LibRaw's highlight-clip mode divides the
multipliers by their minimum, and Lucent's camera colour stage scales scene-linear values by
1 / min(multiplier) (camera_color.hpp, "Exposure zero point").

These are gross-error and regression checks (wrong matrix, swapped channels, white balance,
exposure, geometry), not quality thresholds: the two pipelines differ by design (demosaic
algorithm, DNG profile interpolation versus LibRaw's single matrix). The comparison covers the
scene-referred image; the display rendering and the exported code values are checked against
their definitions by the C++ integration tests (tests/integration/render_fixture_tests.cpp).

When dji-fc4382-dng is selected, two copies of it with the IFD0 Orientation patched to 6 and 8
are compared as well (rows ``dji-fc4382-dng@exif6`` / ``@exif8``; never written to the
statistics): rawpy rotates them itself, so this checks Lucent's orientation handling end to end
against LibRaw's.

Usage::

    render_reference.py [--tier smoke|qualification|all] [--ids id ...] [--lucent-render EXE]
                        [--work-dir research/output/render] [--downscale 8] [--grid 8]
                        [--json-out FILE] [--write-stats tests/integration/data/render_reference.json]
                        [--keep-files]

The work directory keeps each fixture's lucent-render report; the float dumps and exported
images are deleted after the comparison unless --keep-files is given.

Exit status: 0 every selected fixture was compared (a file both decoders refuse counts as
compared), 1 a fixture is missing, fails its hash, or only one side rendered it, 2 usage errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import time
import warnings
from pathlib import Path
from typing import Any

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "fixtures" / "manifest.json"
DEFAULT_WORK_DIR = REPO_ROOT / "research" / "output" / "render"
DEFAULT_STATS = REPO_ROOT / "tests" / "integration" / "data" / "render_reference.json"

STATS_SCHEMA = "lucent.render-reference"
# 2: reference_orientation_exif (from rawpy) replaces orientation_exif (from Lucent's report).
STATS_SCHEMA_VERSION = 2

D65_XY = (0.3127, 0.3290)
SRGB_PRIMARIES = ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060))

# LibRaw flip code -> EXIF orientation, from LibRaw's own table (dcraw, TIFF tag 0x112:
# flip = "50132467"[orientation & 7] - '0'), not from Lucent's code.
FLIP_TO_EXIF = {int("50132467"[exif & 7]): exif for exif in range(1, 9)}

# Synthetic orientation checks: IFD0 Orientation values patched into a copy of this fixture.
ORIENTATION_CHECK_FIXTURE = "dji-fc4382-dng"
ORIENTATION_CHECK_VALUES = (6, 8)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def xy_to_xyz(xy: tuple[float, float]) -> np.ndarray:
    x, y = xy
    return np.array([x / y, 1.0, (1.0 - x - y) / y])


def rgb_to_xyz_matrix(primaries: tuple[tuple[float, float], ...], white: tuple[float, float]) -> np.ndarray:
    """SMPTE RP 177 normalised primary matrix (as lucent::image::rgb_to_xyz)."""
    p = np.stack([xy_to_xyz(c) for c in primaries], axis=1)
    s = np.linalg.solve(p, xy_to_xyz(white))
    return p * s


SRGB_TO_XYZ = rgb_to_xyz_matrix(SRGB_PRIMARIES, D65_XY)
D65_XYZ = xy_to_xyz(D65_XY)


def lab(rgb: np.ndarray) -> np.ndarray:
    """CIELAB (D65 white, Y = 1) of linear sRGB values (..., 3)."""
    xyz = rgb @ SRGB_TO_XYZ.T
    ratio = xyz / D65_XYZ
    delta = 6.0 / 29.0
    f = np.where(ratio > delta**3, np.cbrt(ratio), ratio / (3 * delta**2) + 4.0 / 29.0)
    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1)


def delta_e_2000(lab1: np.ndarray, lab2: np.ndarray) -> np.ndarray:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # colour-science warns that matplotlib is not installed
        import colour

    return np.asarray(colour.delta_E(lab1, lab2, method="CIE 2000"))


def luminance(rgb: np.ndarray) -> np.ndarray:
    return rgb @ SRGB_TO_XYZ[1]


def box_downscale(image: np.ndarray, factor: int) -> np.ndarray:
    h = image.shape[0] // factor * factor
    w = image.shape[1] // factor * factor
    view = image[:h, :w].reshape(h // factor, factor, w // factor, factor, image.shape[2])
    return view.mean(axis=(1, 3), dtype=np.float64)


def region_means(image: np.ndarray, grid: int) -> np.ndarray:
    """Mean of each cell of a grid x grid partition (float64), shape (grid * grid, 3), row-major."""
    h, w = image.shape[:2]
    out = []
    for i in range(grid):
        r0, r1 = (i * h) // grid, ((i + 1) * h) // grid
        for j in range(grid):
            c0, c1 = (j * w) // grid, ((j + 1) * w) // grid
            out.append(image[r0:r1, c0:c1].reshape(-1, 3).mean(axis=0, dtype=np.float64))
    return np.asarray(out)


def read_float_tiff(path: Path) -> np.ndarray:
    """Reads the float TIFF layout written by lucent-render (apps/lucent-render/float_tiff.hpp)."""
    data = path.read_bytes()
    if data[:4] != b"II*\x00":
        raise ValueError(f"{path}: not a little-endian TIFF")
    (ifd,) = struct.unpack_from("<I", data, 4)
    (count,) = struct.unpack_from("<H", data, ifd)
    tags: dict[int, list[int]] = {}
    for i in range(count):
        tag, typ, n, value = struct.unpack_from("<HHII", data, ifd + 2 + 12 * i)
        if typ not in (3, 4):
            continue
        size = 2 if typ == 3 else 4
        fmt = "<" + ("H" if typ == 3 else "I") * n
        offset = ifd + 2 + 12 * i + 8 if n * size <= 4 else value
        tags[tag] = list(struct.unpack_from(fmt, data, offset))
    width, height = tags[256][0], tags[257][0]
    if tags.get(258) != [32, 32, 32] or tags.get(277) != [3] or tags.get(339) != [3, 3, 3] or tags.get(259, [1]) != [1]:
        raise ValueError(f"{path}: not an uncompressed 3 x float32 TIFF")
    image = np.empty((height, width, 3), dtype="<f4")
    rows_per_strip = tags.get(278, [height])[0]
    for s, (offset, nbytes) in enumerate(zip(tags[273], tags[279])):
        rows = nbytes // (width * 12)
        image[s * rows_per_strip : s * rows_per_strip + rows] = np.frombuffer(
            data, dtype="<f4", count=rows * width * 3, offset=offset
        ).reshape(rows, width, 3)
    return image


def find_lucent_render() -> Path | None:
    candidates = sorted(
        (REPO_ROOT / "build").glob("*/bin/lucent-render.exe"), key=lambda p: p.stat().st_mtime, reverse=True
    )
    return candidates[0] if candidates else None


def rawpy_render(path: Path) -> tuple[np.ndarray, int]:
    """rawpy's rendering, upright by LibRaw's own reading of the file's orientation, and that flip."""
    import rawpy

    with rawpy.imread(str(path), shot_select=0) as raw:
        flip = int(raw.sizes.flip)
        rgb = raw.postprocess(
            use_camera_wb=True,
            use_auto_wb=False,
            no_auto_bright=True,
            gamma=(1, 1),
            output_bps=16,
            output_color=rawpy.ColorSpace.sRGB,
            highlight_mode=rawpy.HighlightMode.Clip,
            adjust_maximum_thr=0.0,
        )
    rgb = rgb.astype(np.float32) / np.float32(65535.0)
    if rgb.ndim == 2:
        rgb = rgb[:, :, np.newaxis]
    if rgb.shape[2] == 1:
        # Monochrome: LibRaw writes one channel; a neutral grey is equal in every sRGB channel,
        # which is how Lucent renders monochrome data (camera_color.hpp, MonochromeNeutral).
        rgb = np.repeat(rgb, 3, axis=2)
    return rgb, flip


def rawpy_options() -> dict[str, Any]:
    return {
        "use_camera_wb": True,
        "use_auto_wb": False,
        "no_auto_bright": True,
        "gamma": [1, 1],
        "output_bps": 16,
        "output_color": "sRGB",
        "highlight_mode": "Clip",
        "adjust_maximum_thr": 0.0,
        "demosaic_algorithm": "default (AHD; X-Trans: LibRaw's X-Trans route)",
        "shot_select": 0,
        "user_flip": "not set: LibRaw applies the file's own orientation",
    }


def versions() -> dict[str, str]:
    import rawpy

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        import colour

    return {
        "tool": "research/tools/render_reference.py",
        "rawpy": rawpy.__version__,
        "libraw": ".".join(str(part) for part in rawpy.libraw_version),
        "colour_science": colour.__version__,
        "numpy": np.__version__,
    }


def compare_fixture(
    entry: dict[str, Any], exe: Path, work: Path, downscale: int, grid: int, path: Path | None = None
) -> dict[str, Any]:
    """Compares one fixture (or, with `path`, a derived copy of it that has no manifest hash)."""
    fixture_id = entry["id"]
    derived = path is not None
    path = path or REPO_ROOT / "fixtures" / "cache" / fixture_id / entry["file_name"]
    result: dict[str, Any] = {"id": fixture_id, "tier": entry["tier"], "file_name": path.name}
    if not path.is_file():
        result["status"] = "missing"
        return result
    if not derived and entry.get("sha256") and sha256_file(path) != entry["sha256"]:
        result["status"] = "hash_mismatch"
        return result

    out_dir = work / fixture_id
    if out_dir.exists():
        shutil.rmtree(out_dir)
    (out_dir / "export").mkdir(parents=True)
    linear_path = out_dir / "linear.tiff"
    report_path = out_dir / "report.json"
    start = time.perf_counter()
    proc = subprocess.run(
        [str(exe), str(path), "--out", str(out_dir / "export"), "--subimage", "0", "--legacy-neutral-v0",
         "--dump-linear", str(linear_path), "--report", str(report_path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    result["lucent_seconds"] = round(time.perf_counter() - start, 3)
    lucent_result = json.loads(proc.stdout.strip().splitlines()[-1]) if proc.stdout.strip() else {"status": "error"}

    lucent_ok = lucent_result.get("status") == "ok"
    reference: np.ndarray | None = None
    reference_error = None
    if lucent_ok:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        render = report["render"]
        result["calibration_path"] = render["color"]["path"]
        result["demosaic"] = next(s["parameters"]["route"] for s in render["stages"] if s["stage"] == "demosaic")
        result["lucent_orientation_exif"] = render["orientation_exif"]
        result["display_curve"] = render["rendering"].get("curve")
        result["display_white"] = render["rendering"]["white"]
        result["warnings"] = render["warnings"]
        result["stage_seconds"] = {s["stage"]: round(s["seconds"], 3) for s in render["stages"]}
    else:
        result["lucent_error"] = lucent_result.get("error")
    try:
        reference, flip = rawpy_render(path)
        result["reference_flip"] = flip
        result["reference_orientation_exif"] = FLIP_TO_EXIF[flip]
    except Exception as exc:  # rawpy raises one class per LibRaw error
        reference_error = f"{type(exc).__name__}: {exc}"
        result["reference_error"] = reference_error

    if not lucent_ok and reference is None:
        result["status"] = "both_refused"
        return result
    if not lucent_ok or reference is None:
        result["status"] = "one_side_failed"
        return result

    lucent = read_float_tiff(linear_path)
    np.clip(lucent, 0.0, 1.0, out=lucent)
    result["size"] = [int(lucent.shape[1]), int(lucent.shape[0])]
    if lucent.shape != reference.shape:
        result["status"] = "size_mismatch"
        result["reference_size"] = [int(reference.shape[1]), int(reference.shape[0])]
        return result
    if result["lucent_orientation_exif"] != result["reference_orientation_exif"]:
        result["status"] = "orientation_mismatch"
        return result

    small_l = box_downscale(lucent, downscale)
    small_r = box_downscale(reference, downscale)
    de = delta_e_2000(lab(small_l), lab(small_r)).ravel()
    y_l = luminance(small_l)
    y_r = luminance(small_r)
    result["cell_delta_e"] = {
        "mean": round(float(de.mean()), 4),
        "p95": round(float(np.percentile(de, 95)), 4),
        "max": round(float(de.max()), 4),
        "cells": int(de.size),
    }
    result["luminance_ratio"] = round(float(y_l.mean() / y_r.mean()), 5)

    regions_l = region_means(lucent, grid)
    regions_r = region_means(reference, grid)
    region_de = delta_e_2000(lab(regions_l), lab(regions_r))
    result["region_delta_e"] = {"mean": round(float(region_de.mean()), 4), "max": round(float(region_de.max()), 4)}
    result["region_luminance_ratio"] = round(float(luminance(regions_l).mean() / luminance(regions_r).mean()), 5)
    result["reference_region_means"] = [[round(float(v), 7) for v in cell] for cell in regions_r]
    result["status"] = "compared"
    return result


def patch_orientation(source: Path, directory: Path, value: int) -> Path:
    """A copy of a TIFF-based raw file whose IFD0 Orientation (SHORT, count 1) is `value`."""
    data = bytearray(source.read_bytes())
    end = "<" if data[:2] == b"II" else ">"
    (ifd,) = struct.unpack_from(end + "I", data, 4)
    (count,) = struct.unpack_from(end + "H", data, ifd)
    for i in range(count):
        entry = ifd + 2 + 12 * i
        tag, typ, n = struct.unpack_from(end + "HHI", data, entry)
        if tag == 0x0112 and typ == 3 and n == 1:
            struct.pack_into(end + "H", data, entry + 8, value)
            break
    else:
        raise ValueError(f"{source}: no IFD0 Orientation tag to patch")
    directory.mkdir(parents=True, exist_ok=True)
    out = directory / f"{source.stem}_exif{value}{source.suffix}"
    out.write_bytes(data)
    return out


def format_stats(stats: dict[str, Any]) -> str:
    """Indented header, one compact line per fixture (small file, one-line diffs per fixture)."""
    head = json.dumps({k: v for k, v in stats.items() if k != "fixtures"}, indent=1)
    items = [f"  {json.dumps(k)}: {json.dumps(v, separators=(',', ':'))}" for k, v in stats["fixtures"].items()]
    return head[: head.rindex("}")].rstrip() + ',\n "fixtures": {\n' + ",\n".join(items) + "\n }\n}\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--tier", choices=["smoke", "qualification", "all"], default="smoke")
    parser.add_argument("--ids", nargs="*", default=None)
    parser.add_argument("--lucent-render", type=Path, default=None)
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR)
    parser.add_argument("--downscale", type=int, default=8)
    parser.add_argument("--grid", type=int, default=8)
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--write-stats", type=Path, nargs="?", const=DEFAULT_STATS, default=None)
    parser.add_argument(
        "--keep-files", action="store_true", help="keep the float dumps and exported files in the work directory"
    )
    args = parser.parse_args(argv)

    exe = args.lucent_render or find_lucent_render()
    if exe is None or not exe.is_file():
        print("render_reference: lucent-render.exe not found; build it or pass --lucent-render", file=sys.stderr)
        return 2
    if args.downscale < 1 or args.grid < 1:
        print("render_reference: --downscale and --grid must be positive", file=sys.stderr)
        return 2
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    entries = [
        e
        for e in manifest["fixtures"]
        if (args.ids is None or e["id"] in args.ids) and (args.tier == "all" or args.ids or e["tier"] == args.tier)
    ]
    if args.ids:
        unknown = sorted(set(args.ids) - {e["id"] for e in entries})
        if unknown:
            print(f"render_reference: unknown fixture ids {unknown}", file=sys.stderr)
            return 2

    args.work_dir.mkdir(parents=True, exist_ok=True)
    results = []
    failed = False
    header = f"{'fixture':44} {'path':20} {'dE mean':>8} {'dE p95':>7} {'dE max':>7} {'Y ratio':>8} {'reg dE':>7} {'status'}"
    print(header)
    jobs: list[tuple[dict[str, Any], Path | None]] = [(e, None) for e in entries]
    for entry in entries:
        source = REPO_ROOT / "fixtures" / "cache" / entry["id"] / entry["file_name"]
        if entry["id"] == ORIENTATION_CHECK_FIXTURE and source.is_file():
            for value in ORIENTATION_CHECK_VALUES:
                derived = {**entry, "id": f"{entry['id']}@exif{value}"}
                jobs.append((derived, patch_orientation(source, args.work_dir / "derived", value)))
    for entry, derived_path in jobs:
        r = compare_fixture(entry, exe, args.work_dir, args.downscale, args.grid, derived_path)
        if derived_path is not None:
            r["derived"] = True
            derived_path.unlink(missing_ok=True)
        if not args.keep_files:
            # The float dumps are about 12 bytes per pixel (540 MB for 45 MP); keep only reports.
            fixture_dir = args.work_dir / entry["id"]
            (fixture_dir / "linear.tiff").unlink(missing_ok=True)
            shutil.rmtree(fixture_dir / "export", ignore_errors=True)
        results.append(r)
        if r["status"] in ("missing", "hash_mismatch", "one_side_failed", "size_mismatch", "orientation_mismatch"):
            failed = True
        if r["status"] == "compared":
            print(
                f"{r['id']:44} {r['calibration_path']:20} {r['cell_delta_e']['mean']:8.3f} {r['cell_delta_e']['p95']:7.3f} "
                f"{r['cell_delta_e']['max']:7.2f} {r['luminance_ratio']:8.4f} {r['region_delta_e']['mean']:7.3f} compared"
            )
        else:
            detail = r.get("lucent_error") or r.get("reference_error") or ""
            print(f"{r['id']:44} {'':20} {'':>8} {'':>7} {'':>7} {'':>8} {'':>7} {r['status']} {detail}")
        sys.stdout.flush()

    summary = {
        "generator": versions(),
        "lucent_render": str(exe),
        "rawpy_options": rawpy_options(),
        "downscale": args.downscale,
        "grid": args.grid,
        "results": results,
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    if args.write_stats:
        stats_path: Path = args.write_stats
        existing: dict[str, Any] = {}
        if stats_path.is_file():
            existing = json.loads(stats_path.read_text(encoding="utf-8")).get("fixtures", {})
        for r in results:
            if r.get("derived"):
                continue
            if r["status"] == "both_refused":
                # Both decoders refuse the file: the tests check that Lucent still refuses it.
                existing[r["id"]] = {
                    "refused": {
                        "lucent_error_code": (r.get("lucent_error") or {}).get("code"),
                        "reference_error": r.get("reference_error"),
                    }
                }
                continue
            if r["status"] != "compared":
                continue
            existing[r["id"]] = {
                "size": r["size"],
                "reference_orientation_exif": r["reference_orientation_exif"],
                "subimage": 0,
                "reference_region_means": r["reference_region_means"],
                "measured": {
                    "calibration_path": r["calibration_path"],
                    "region_delta_e_mean": r["region_delta_e"]["mean"],
                    "region_delta_e_max": r["region_delta_e"]["max"],
                    "region_luminance_ratio": r["region_luminance_ratio"],
                    "cell_delta_e_mean": r["cell_delta_e"]["mean"],
                    "cell_delta_e_p95": r["cell_delta_e"]["p95"],
                    "luminance_ratio": r["luminance_ratio"],
                },
            }
        stats = {
            "schema": STATS_SCHEMA,
            "schema_version": STATS_SCHEMA_VERSION,
            "generator": versions(),
            "rawpy_options": rawpy_options(),
            "grid": args.grid,
            "encoding": "linear sRGB primaries (D65), each pixel clipped to [0, 1], mean per grid cell, row-major",
            "fixtures": dict(sorted(existing.items())),
        }
        stats_path.parent.mkdir(parents=True, exist_ok=True)
        stats_path.write_text(format_stats(stats), encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
