#!/usr/bin/env python3
"""Independent colour reference for camera qualification: colour charts against their published
values (ADR 0020).

``fixtures/qualification/charts.json`` (schema ``lucent.qualification-charts`` v1) defines, for
every corpus fixture that shows a colour chart, the chart's published reference values (L*a*b*,
D50, CIE 1931 2 degree observer, from the manufacturer, with URL and retrieval date) and a sample
box per patch in the fixture's camera-area frame (the frame Lucent's develop render outputs:
geometry version 2, ADR 0017). This tool measures the charts of renderings that do NOT use
Lucent's code and writes the evidence the C++ check ``colour_reference`` (apps/lucent-qualify)
is compared with:

* ``rawpy``: LibRaw's own rendering through rawpy (``postprocess``: as-shot white balance, no
  automatic brightening, linear output, 16 bits, LibRaw's own camera matrix to CIE XYZ
  (``output_color=XYZ``, D65-relative), highlight clip, LibRaw's default demosaic, no user flip:
  LibRaw applies the file's orientation). It shares the decoder and the camera matrix with
  Lucent's decoder-matrix path but none of Lucent's code (white balance, demosaicing, colour
  stages, working space, sampling, colour science);
* ``lucent-w2`` (optional, ``--lucent-render``): Lucent's documented scene-linear dump
  (``lucent-render --legacy-neutral-v0 --dump-linear``, camera area applied), as a cross-check of
  the C++ check's own numbers (the develop path's operation 1, which the W2 path equals up to the
  exposure gain).

Measurement (identical in the C++ check, apps/lucent-qualify/colour_reference.cpp):

1. Per patch, the mean linear value over its sample box (the rawpy image is LibRaw's whole
   visible area: the box is shifted by the frame's origin in it), converted to CIE XYZ relative
   to the rendering's white (D65 for every rendering here).
2. Chromatic adaptation from the rendering's white (D65) to the reference white (D50), Bradford
   (Lam 1985, the linearised form of ICC.1 Annex E).
3. Exposure normalisation: one scale for all patches, so that the luminance Y of the chart's
   normalisation patch (a light grey, see charts.json) equals its reference Y (D50 white, Y = 1).
4. CIELAB relative to D50 (Y = 1) and CIEDE2000 (kL = kC = kH = 1) against the reference.

Reported per rendering: per-patch L*a*b* and CIEDE2000, mean, 90th percentile (linear
interpolation between order statistics, numpy's default; the C++ check uses the same rule) and
maximum, and two diagnostics that are not gated: "chart_balanced" (steps 2 and 3 replaced by a
Bradford von Kries scaling that maps the normalisation patch exactly onto its reference XYZ, which
removes the as-shot white balance and exposure and leaves the camera matrix's own error) and, for
fixtures with ``alternate_references``, the as-shot statistics against those references.

Subcommands::

    chart_reference.py measure [--ids ID ...] [--lucent-render EXE] [--json-out FILE] [--write-charts]
    chart_reference.py locate --image FILE --rows R --cols C --ids-grid "1A,1B;2A,2B"
                              --corners "x,y x,y x,y x,y" [--box-fraction 0.5] [--overlay FILE]

``--write-charts`` records the rawpy statistics as each fixture's ``independent_measurement`` in
charts.json (the C++ check reports them beside Lucent's; they are never gated on).

``locate`` helps a reviewer add a chart: from the centres of the four corner patches of one
patch grid (top-left, top-right, bottom-left, bottom-right, in the image's pixels; read them off
a preview) it refines every patch's edges on the image (a ``--dump-linear`` float TIFF of the
camera-area frame, or any 8/16-bit image of it), fits the grid again through the patches whose
size is typical, and prints sample boxes (the central ``--box-fraction`` of the median patch
side) as JSON for charts.json; ``--overlay`` draws them for the visual review the chart entry
records.

Exit status: 0 every selected chart fixture was measured, 1 a fixture is missing, fails its
manifest hash or could not be rendered, 2 usage errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import warnings
from pathlib import Path
from typing import Any

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CHARTS = REPO_ROOT / "fixtures" / "qualification" / "charts.json"
DEFAULT_MANIFEST = REPO_ROOT / "fixtures" / "manifest.json"
CHARTS_SCHEMA = "lucent.qualification-charts"
CHARTS_SCHEMA_VERSION = 1

# CIE 1931 2 degree chromaticities, as Lucent's color_math.hpp and colour-science define them.
D65_XY = (0.3127, 0.3290)
D50_XY = (0.3457, 0.3585)
SRGB_PRIMARIES = ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060))


def colour_module():
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # colour-science warns that matplotlib is not installed
        import colour

    return colour


def xy_to_xyz(xy: tuple[float, float]) -> np.ndarray:
    x, y = xy
    return np.array([x / y, 1.0, (1.0 - x - y) / y])


def rgb_to_xyz_matrix(primaries, white) -> np.ndarray:
    p = np.stack([xy_to_xyz(c) for c in primaries], axis=1)
    return p * np.linalg.solve(p, xy_to_xyz(white))


SRGB_TO_XYZ = rgb_to_xyz_matrix(SRGB_PRIMARIES, D65_XY)
D65 = xy_to_xyz(D65_XY)
D50 = xy_to_xyz(D50_XY)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ---- Chart data --------------------------------------------------------------------------------


def load_charts(path: Path) -> dict[str, Any]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    if doc.get("schema") != CHARTS_SCHEMA or doc.get("schema_version") != CHARTS_SCHEMA_VERSION:
        raise ValueError(f"{path}: not a {CHARTS_SCHEMA} v{CHARTS_SCHEMA_VERSION} document")
    for fixture in doc["fixtures"]:
        ref = doc["references"][fixture["reference"]]
        missing = sorted(set(ref["patches"]) - set(fixture["patches"]))
        extra = sorted(set(fixture["patches"]) - set(ref["patches"]))
        if extra or (missing and not set(missing) <= set(fixture.get("excluded", {}))):
            raise ValueError(f"{fixture['id']}: patches {missing} lack a box, {extra} are not in the reference")
        if fixture["normalisation_patch"] not in fixture["patches"]:
            raise ValueError(f"{fixture['id']}: the normalisation patch has no box")
    return doc


def reference_lab(reference: dict[str, Any], patch_ids: list[str]) -> np.ndarray:
    return np.array([reference["patches"][p]["lab"] for p in patch_ids], dtype=float)


# ---- Colour science (colour-science, independent of Lucent's C++) -------------------------------


def lab_to_xyz(lab: np.ndarray, white: np.ndarray) -> np.ndarray:
    colour = colour_module()
    return np.asarray(colour.Lab_to_XYZ(lab, colour.XYZ_to_xy(white)))


def xyz_to_lab(xyz: np.ndarray, white: np.ndarray) -> np.ndarray:
    colour = colour_module()
    return np.asarray(colour.XYZ_to_Lab(xyz, colour.XYZ_to_xy(white)))


def bradford(xyz: np.ndarray, src_white: np.ndarray, dst_white: np.ndarray) -> np.ndarray:
    colour = colour_module()
    return np.asarray(colour.adaptation.chromatic_adaptation_VonKries(xyz, src_white, dst_white, transform="Bradford"))


def ciede2000(lab1: np.ndarray, lab2: np.ndarray) -> np.ndarray:
    colour = colour_module()
    return np.asarray(colour.delta_E(lab1, lab2, method="CIE 2000"))


def stats(de: np.ndarray) -> dict[str, float]:
    return {
        "mean": round(float(np.mean(de)), 4),
        "p90": round(float(np.percentile(de, 90)), 4),
        "max": round(float(np.max(de)), 4),
    }


def evaluate(xyz_means: np.ndarray, patch_ids: list[str], ref_lab: np.ndarray, norm_patch: str,
             render_white: np.ndarray) -> dict[str, Any]:
    """Steps 2-4 of the module docstring for patch means in XYZ relative to `render_white`."""
    i = patch_ids.index(norm_patch)
    ref_xyz = lab_to_xyz(ref_lab, D50)
    adapted = bradford(xyz_means, render_white, D50)
    scale = ref_xyz[i, 1] / adapted[i, 1]
    as_shot_lab = xyz_to_lab(adapted * scale, D50)
    de = ciede2000(as_shot_lab, ref_lab)
    # Diagnostic: balance on the normalisation patch (maps it exactly onto its reference).
    balanced_lab = xyz_to_lab(bradford(xyz_means, xyz_means[i], ref_xyz[i]), D50)
    de_bal = ciede2000(balanced_lab, ref_lab)
    return {
        "normalisation_scale": float(scale),
        "as_shot": {**stats(de), "patches": {p: {"lab": [round(float(v), 3) for v in lab], "delta_e": round(float(d), 4)}
                                             for p, lab, d in zip(patch_ids, as_shot_lab, de)}},
        "chart_balanced": {**stats(de_bal), "patches": {p: {"lab": [round(float(v), 3) for v in lab],
                                                            "delta_e": round(float(d), 4)}
                                                        for p, lab, d in zip(patch_ids, balanced_lab, de_bal)}},
        "_de": de,
    }


# ---- Renderings --------------------------------------------------------------------------------


# LibRaw's (dcraw's) sRGB -> XYZ (D65) constant, LibRaw_constants::xyz_rgb.
LIBRAW_XYZ_RGB = np.array([[0.412453, 0.357580, 0.180423],
                           [0.212671, 0.715160, 0.072169],
                           [0.019334, 0.119193, 0.950227]])


def libraw_camera_to_xyz(cam_xyz: np.ndarray) -> np.ndarray:
    """XYZ (D65) from white-balanced camera RGB with LibRaw's own matrix: LibRaw's cam_xyz_coeff()
    (dcraw) turns the XYZ -> camera matrix into cam_rgb = cam_xyz * xyz_rgb with every row
    normalised to sum 1 and rgb_cam = pseudo-inverse(cam_rgb); its sRGB output is rgb_cam * camera,
    its XYZ output xyz_rgb * rgb_cam * camera. Applied here in floating point so that bright
    bluish whites are not clipped by LibRaw's 16-bit output stage (Z > 1)."""
    cam_rgb = cam_xyz[:3] @ LIBRAW_XYZ_RGB
    cam_rgb = cam_rgb / cam_rgb.sum(axis=1, keepdims=True)
    return LIBRAW_XYZ_RGB @ np.linalg.pinv(cam_rgb)


def rawpy_camera(path: Path) -> tuple[np.ndarray, np.ndarray, int]:
    """LibRaw's rendering: white-balanced, demosaiced camera RGB (float, 1.0 = the lowest
    white-balanced saturation, where LibRaw's highlight clip limits every channel), upright by
    LibRaw's own orientation, with LibRaw's camera -> XYZ (D65) matrix and its flip code."""
    import rawpy

    with rawpy.imread(str(path), shot_select=0) as raw:
        flip = int(raw.sizes.flip)
        matrix = libraw_camera_to_xyz(np.array(raw.rgb_xyz_matrix, dtype=np.float64))
        camera = raw.postprocess(
            use_camera_wb=True,
            use_auto_wb=False,
            no_auto_bright=True,
            gamma=(1, 1),
            output_bps=16,
            output_color=rawpy.ColorSpace.raw,
            highlight_mode=rawpy.HighlightMode.Clip,
            adjust_maximum_thr=0.0,
        )
    return camera.astype(np.float64) / 65535.0, matrix, flip


def rawpy_options() -> dict[str, Any]:
    return {
        "use_camera_wb": True, "use_auto_wb": False, "no_auto_bright": True, "gamma": [1, 1],
        "output_bps": 16, "output_color": "raw (white-balanced camera RGB); LibRaw's own camera matrix "
                                          "(rgb_xyz_matrix through cam_xyz_coeff) applied in floating point",
        "highlight_mode": "Clip", "adjust_maximum_thr": 0.0,
        "demosaic_algorithm": "default (AHD; X-Trans: LibRaw's X-Trans route)", "shot_select": 0,
        "user_flip": "not set: LibRaw applies the file's own orientation",
    }


def lucent_w2_xyz(exe: Path, path: Path, work: Path) -> np.ndarray:
    """Lucent's W2 scene-linear dump with the camera area applied, as XYZ (D65-relative)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from render_reference import read_float_tiff

    out = work / "export"
    out.mkdir(parents=True, exist_ok=True)
    dump = work / "linear.tiff"
    proc = subprocess.run([str(exe), str(path), "--out", str(out), "--legacy-neutral-v0", "--dump-linear", str(dump)],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0 or not dump.is_file():
        raise RuntimeError(f"lucent-render failed: {proc.stdout.strip()} {proc.stderr.strip()}")
    rgb = read_float_tiff(dump).astype(np.float64)
    dump.unlink()
    return rgb @ SRGB_TO_XYZ.T


# A patch is excluded from the statistics when more than this fraction of its box's samples reached
# the white-balanced clip level (the C++ check uses the same rule on Lucent's own stage-one image).
CLIPPED_FRACTION_LIMIT = 0.001


def box_means(image: np.ndarray, boxes: dict[str, list[int]], patch_ids: list[str], origin: tuple[int, int],
              clip_level: float | None = None) -> tuple[np.ndarray, dict[str, float]]:
    """Mean of every patch's box (shifted by `origin`) and, with `clip_level`, the fraction of each
    box's samples at or above it (only patches with clipped samples are listed)."""
    out = []
    clipped = {}
    for p in patch_ids:
        x, y, w, h = boxes[p]
        x += origin[0]
        y += origin[1]
        if x < 0 or y < 0 or x + w > image.shape[1] or y + h > image.shape[0]:
            raise ValueError(f"patch {p}: box outside the image")
        region = image[y:y + h, x:x + w].reshape(-1, image.shape[2])
        out.append(region.mean(axis=0))
        if clip_level is not None:
            fraction = float(np.count_nonzero(region >= clip_level)) / region.size
            if fraction > 0.0:
                clipped[p] = round(fraction, 6)
    return np.asarray(out), clipped


def versions() -> dict[str, str]:
    import rawpy

    return {
        "tool": "research/tools/chart_reference.py",
        "rawpy": rawpy.__version__,
        "libraw": ".".join(str(v) for v in rawpy.libraw_version),
        "colour_science": colour_module().__version__,
        "numpy": np.__version__,
    }


def format_charts(value: Any, indent: int = 0) -> str:
    """charts.json's layout: objects one member per line (one more space per level), arrays of
    numbers on one line."""
    pad = " " * indent
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = [f"{pad} {json.dumps(k)}: {format_charts(v, indent + 1)}" for k, v in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(value, list) and all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in value):
        return json.dumps(value)
    if isinstance(value, list):
        if not value:
            return "[]"
        items = [f"{pad} {format_charts(v, indent + 1)}" for v in value]
        return "[\n" + ",\n".join(items) + "\n" + pad + "]"
    return json.dumps(value, ensure_ascii=False)


def write_independent_measurements(path: Path, results: list[dict[str, Any]], generator: dict[str, str]) -> None:
    """Records each measured fixture's rawpy statistics as its independent_measurement."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    renderer = (f"rawpy {generator['rawpy']} (LibRaw {generator['libraw']}): as-shot white balance, LibRaw's demosaic "
                f"and highlight clip, LibRaw's camera matrix; colour-science {generator['colour_science']}")
    today = __import__("datetime").date.today().isoformat()
    by_id = {r["id"]: r for r in results if r.get("status") == "measured"}
    for fixture in doc["fixtures"]:
        r = by_id.get(fixture["id"])
        if r is None:
            continue
        rawpy_result = r["renderings"]["rawpy"]
        fixture["independent_measurement"] = {
            "renderer": renderer,
            "measured": today,
            "as_shot": {k: rawpy_result["as_shot"][k] for k in ("mean", "p90", "max")},
            "chart_balanced": {k: rawpy_result["chart_balanced"][k] for k in ("mean", "p90", "max")},
        }
    path.write_text(format_charts(doc) + "\n", encoding="utf-8", newline="\n")


def measure(args: argparse.Namespace) -> int:
    charts = load_charts(args.charts)
    manifest = {f["id"]: f for f in json.loads(args.manifest.read_text(encoding="utf-8"))["fixtures"]}
    selected = [f for f in charts["fixtures"] if not args.ids or f["id"] in args.ids]
    if args.ids and len(selected) != len(set(args.ids)):
        print(f"chart_reference: unknown chart fixtures {sorted(set(args.ids) - {f['id'] for f in selected})}",
              file=sys.stderr)
        return 2
    results = []
    failed = False
    for fixture in selected:
        entry = manifest.get(fixture["id"])
        result: dict[str, Any] = {"id": fixture["id"], "reference": fixture["reference"]}
        results.append(result)
        if entry is None:
            result["status"] = "not_in_manifest"
            failed = True
            continue
        path = REPO_ROOT / "fixtures" / "cache" / entry["id"] / entry["file_name"]
        if not path.is_file():
            result["status"] = "missing"
            failed = True
            continue
        if sha256_file(path) != entry["sha256"]:
            result["status"] = "hash_mismatch"
            failed = True
            continue
        reference = charts["references"][fixture["reference"]]
        boxed = [p for p in reference["patches"] if p in fixture["patches"]]
        frame = fixture["frame"]
        renderings: dict[str, Any] = {}
        camera, matrix, flip = rawpy_camera(path)
        camera_means, clipped = box_means(camera, fixture["patches"], boxed, tuple(frame["origin"]), clip_level=1.0)
        excluded = {p: f for p, f in clipped.items() if f > CLIPPED_FRACTION_LIMIT}
        if fixture["normalisation_patch"] in excluded:
            raise ValueError(f"{fixture['id']}: the normalisation patch is clipped")
        patch_ids = [p for p in boxed if p not in excluded]
        keep = [boxed.index(p) for p in patch_ids]
        ref_lab = reference_lab(reference, patch_ids)
        means = camera_means[keep] @ matrix.T
        renderings["rawpy"] = evaluate(means, patch_ids, ref_lab, fixture["normalisation_patch"], D65)
        renderings["rawpy"]["image_size"] = [int(camera.shape[1]), int(camera.shape[0])]
        renderings["rawpy"]["flip"] = flip
        renderings["rawpy"]["clipped_fraction"] = clipped
        renderings["rawpy"]["camera_to_xyz"] = [[round(float(v), 6) for v in row] for row in matrix]
        for alt in fixture.get("alternate_references", []):
            alt_ref = charts["references"][alt]
            alt_eval = evaluate(means, patch_ids, reference_lab(alt_ref, patch_ids), fixture["normalisation_patch"], D65)
            renderings["rawpy"].setdefault("alternate_references", {})[alt] = {
                "as_shot": {k: alt_eval["as_shot"][k] for k in ("mean", "p90", "max")},
                "chart_balanced": {k: alt_eval["chart_balanced"][k] for k in ("mean", "p90", "max")}}
        result["excluded_patches"] = excluded
        del camera
        if args.lucent_render:
            with tempfile.TemporaryDirectory(prefix="chart-ref-") as tmp:
                lxyz = lucent_w2_xyz(args.lucent_render, path, Path(tmp))
            if [lxyz.shape[1], lxyz.shape[0]] != frame["size"]:
                raise ValueError(f"{fixture['id']}: the dump is {lxyz.shape[1]}x{lxyz.shape[0]}, the frame {frame['size']}")
            lmeans, _ = box_means(lxyz, fixture["patches"], patch_ids, (0, 0))
            renderings["lucent-w2"] = evaluate(lmeans, patch_ids, ref_lab, fixture["normalisation_patch"], D65)
            del lxyz
        for r in renderings.values():
            r.pop("_de", None)
        result["status"] = "measured"
        result["renderings"] = renderings
        line = f"{fixture['id']:40}"
        for name, r in renderings.items():
            s = r["as_shot"]
            line += f"  {name}: mean {s['mean']:.2f} p90 {s['p90']:.2f} max {s['max']:.2f} (balanced mean {r['chart_balanced']['mean']:.2f})"
        print(line)
        sys.stdout.flush()
    summary = {"generator": versions(), "rawpy_options": rawpy_options(), "charts": str(args.charts.relative_to(REPO_ROOT))
               if args.charts.is_relative_to(REPO_ROOT) else str(args.charts), "results": results}
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    if args.write_charts and not failed:
        write_independent_measurements(args.charts, results, summary["generator"])
    return 1 if failed else 0


# ---- Locating patches --------------------------------------------------------------------------


def load_image(path: Path) -> np.ndarray:
    if path.suffix.lower() in (".tif", ".tiff"):
        try:
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from render_reference import read_float_tiff

            return read_float_tiff(path).astype(np.float64)
        except ValueError:
            pass
    from PIL import Image

    image = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return (image / 255.0) ** 2.2


def bilinear_grid(corners: list[tuple[float, float]], rows: int, cols: int) -> dict[tuple[int, int], np.ndarray]:
    tl, tr, bl, br = (np.array(c, dtype=float) for c in corners)
    grid = {}
    for r in range(rows):
        v = r / (rows - 1) if rows > 1 else 0.0
        left, right = tl + (bl - tl) * v, tr + (br - tr) * v
        for c in range(cols):
            u = c / (cols - 1) if cols > 1 else 0.0
            grid[(r, c)] = left + (right - left) * u
    return grid


def patch_edges(lum: np.ndarray, centre: np.ndarray, reach: int, tolerance: float = 0.12) -> list[int | None]:
    """Distances from `centre` to the patch's left, right, top and bottom edges: the first of three
    consecutive positions whose mean luminance (over a short perpendicular segment) departs from
    the patch median by more than `tolerance` (relative)."""
    cx, cy = int(round(centre[0])), int(round(centre[1]))
    w = max(4, reach // 12)
    ref = float(np.median(lum[cy - w:cy + w, cx - w:cx + w]))
    edges: list[int | None] = []
    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        run, found = 0, None
        for k in range(w, 2 * reach):
            x, y = cx + dx * k, cy + dy * k
            if not (w <= x < lum.shape[1] - w and w <= y < lum.shape[0] - w):
                break
            v = float(np.mean(lum[y - w:y + w, x] if dx else lum[y, x - w:x + w]))
            run = run + 1 if abs(v - ref) > tolerance * max(ref, 1e-6) else 0
            if run == 3:
                found = k - 2
                break
        edges.append(found)
    return edges


def locate(args: argparse.Namespace) -> int:
    image = load_image(args.image)
    lum = image @ np.array([0.2126, 0.7152, 0.0722])
    ids = [row.split(",") for row in args.ids_grid.split(";")]
    if len(ids) != args.rows or any(len(r) != args.cols for r in ids):
        print("chart_reference: --ids-grid does not have --rows x --cols entries", file=sys.stderr)
        return 2
    corners = [tuple(float(v) for v in c.split(",")) for c in args.corners.split()]
    if len(corners) != 4:
        print("chart_reference: --corners needs four x,y centres", file=sys.stderr)
        return 2
    grid = bilinear_grid(corners, args.rows, args.cols)
    pitch = min(np.linalg.norm(grid[(0, 1)] - grid[(0, 0)]) if args.cols > 1 else 1e9,
                np.linalg.norm(grid[(1, 0)] - grid[(0, 0)]) if args.rows > 1 else 1e9)
    reach = int(pitch * 0.45)
    found: dict[tuple[int, int], tuple[np.ndarray, np.ndarray]] = {}
    for key, centre in grid.items():
        e = patch_edges(lum, centre, reach)
        if None in e:
            continue
        size = np.array([e[0] + e[1], e[2] + e[3]], dtype=float)
        found[key] = (centre + np.array([(e[1] - e[0]) / 2.0, (e[3] - e[2]) / 2.0]), size)
    if not found:
        print("chart_reference: no patch edge found; check the corners", file=sys.stderr)
        return 1
    median = np.median(np.array([s for _, s in found.values()]), axis=0)
    typical = {k: c for k, (c, s) in found.items() if np.all(np.abs(s - median) <= 0.05 * median)}
    # Least-squares bilinear model x, y = a + b r + c k + d r k through the typical patches.
    keys = sorted(typical)
    design = np.array([[1.0, r, c, r * c] for r, c in keys])
    coeff, *_ = np.linalg.lstsq(design, np.array([typical[k] for k in keys]), rcond=None)
    side = int(round(float(np.median(median)) * args.box_fraction))
    side -= side % 2
    boxes = {}
    residuals = []
    for (r, c) in grid:
        centre = np.array([1.0, r, c, r * c]) @ coeff
        if (r, c) in typical:
            residuals.append(float(np.linalg.norm(typical[(r, c)] - centre)))
        boxes[ids[r][c]] = [int(round(centre[0] - side / 2)), int(round(centre[1] - side / 2)), side, side]
    print(json.dumps({"patch_side_median": [float(v) for v in median], "typical_patches": len(typical),
                      "fit_residual_max_px": round(max(residuals), 2), "box_side": side, "boxes": boxes}, indent=1))
    if args.overlay:
        from PIL import Image, ImageDraw

        display = np.clip(image / max(float(np.percentile(image, 99.5)), 1e-9), 0.0, 1.0) ** (1 / 2.2)
        picture = Image.fromarray((display * 255).astype(np.uint8))
        draw = ImageDraw.Draw(picture)
        for pid, (x, y, w, h) in boxes.items():
            draw.rectangle([x, y, x + w, y + h], outline=(255, 0, 0), width=max(2, side // 30))
            draw.text((x + 4, y + 4), pid, fill=(255, 255, 0))
        xs = [b[0] for b in boxes.values()] + [b[0] + b[2] for b in boxes.values()]
        ys = [b[1] for b in boxes.values()] + [b[1] + b[3] for b in boxes.values()]
        margin = side
        picture = picture.crop((max(0, min(xs) - margin), max(0, min(ys) - margin),
                                min(picture.width, max(xs) + margin), min(picture.height, max(ys) + margin)))
        picture.thumbnail((1600, 1600))
        picture.save(args.overlay, quality=90)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("measure", help="measure the charts of the corpus fixtures")
    m.add_argument("--charts", type=Path, default=DEFAULT_CHARTS)
    m.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    m.add_argument("--ids", nargs="*", default=None)
    m.add_argument("--lucent-render", type=Path, default=None, help="also measure Lucent's W2 scene-linear dump")
    m.add_argument("--json-out", type=Path, default=None)
    m.add_argument("--write-charts", action="store_true",
                   help="record the rawpy statistics as each fixture's independent_measurement in --charts")
    loc = sub.add_parser("locate", help="sample boxes of one patch grid from its four corner patch centres")
    loc.add_argument("--image", type=Path, required=True)
    loc.add_argument("--rows", type=int, required=True)
    loc.add_argument("--cols", type=int, required=True)
    loc.add_argument("--ids-grid", required=True, help='patch ids row by row: "1A,1B,1C;2A,2B,2C"')
    loc.add_argument("--corners", required=True, help='"x,y x,y x,y x,y": top-left, top-right, bottom-left, bottom-right')
    loc.add_argument("--box-fraction", type=float, default=0.5)
    loc.add_argument("--overlay", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        return measure(args) if args.command == "measure" else locate(args)
    except (ValueError, RuntimeError, KeyError) as exc:
        print(f"chart_reference: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
