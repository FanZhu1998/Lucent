#!/usr/bin/env python3
"""rawpy/LibRaw reference probe for Lucent's decoder-parity tests (blueprint Appendix D1).

For each input RAW file this writes a probe report (schema ``lucent.probe-report`` version 1).
Its ``parity`` block holds values that Lucent's native LibRaw adapter must reproduce; the
semantics of every field are exactly those of the rawpy property of the same name (rawpy
0.27.1, ``rawpy/_rawpy.pyx`` and ``rawpy/data_helper.h``), read after ``LibRaw::unpack()``:

========================== ======================================================================
field                      rawpy source / LibRaw data
========================== ======================================================================
raw_width ... flip         ``RawPy.sizes`` -> ``imgdata.sizes`` (read after unpack)
num_colors                 ``imgdata.idata.colors``
color_desc                 ``imgdata.idata.cdesc`` (ASCII)
raw_pattern                ``RawPy.raw_pattern``: ``LibRaw::COLOR(y - top_margin, x - left_margin)``
                           over n x n (n = 1 when filters == 0, 16 when filters == 1, 6 for X-Trans,
                           else 4 reduced to 2 x 2 when periodic); ``null`` when ``raw_image`` is NULL
black_level_per_channel    rawpy ``adjust_bl_()`` on ``imgdata.rawdata.color`` (black + cblack[0..3]
                           + the cblack[4], [5], [6..] pattern folded in as LibRaw::adjust_bl does)
white_level                ``imgdata.rawdata.color.maximum``
camera_white_level_...     ``imgdata.rawdata.color.linear_max[0..3]``; ``null`` unless all four > 0
camera_whitebalance        ``imgdata.rawdata.color.cam_mul[0..3]``
daylight_whitebalance      ``imgdata.rawdata.color.pre_mul[0..3]``
rgb_xyz_matrix             ``imgdata.rawdata.color.cam_xyz`` (4 x 3)
color_matrix               ``imgdata.rawdata.color.cmatrix`` (3 x 4) -- NOT ``rgb_cam``
raw_type                   ``"flat"`` when ``imgdata.rawdata.raw_image`` is non-NULL, else ``"stack"``
raw_data_sha256            ``--decode`` only, flat raws only: SHA-256 of ``raw_image`` as
                           raw_height x raw_width uint16 little-endian samples, row-major, no padding
========================== ======================================================================

Floats are the float32 LibRaw values widened exactly to double. A non-finite float is written
as the string ``"nan"``, ``"inf"`` or ``"-inf"`` (JSON has no such numbers).

Usage::

    probe_reference.py [--decode] FILE...                    # report(s) to stdout
    probe_reference.py --decode --manifest fixtures/manifest.json --tier all \\
        --out-dir research/output/reference --parity-dir fixtures/reference

In manifest mode each cached fixture (``fixtures/cache/<id>/<file_name>``) is hashed and checked
against the manifest before it is probed; the report is named after the fixture id.
A LibRaw failure is a result, not a tool failure: it is recorded as ``"status": "error"`` with
``"parity": null``. Exit status: 0 when every input was probed, 1 when an input was missing,
failed its manifest hash or crashed the probe, 2 for usage errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np
import rawpy

SCHEMA = "lucent.probe-report"
SCHEMA_VERSION = 1
MANIFEST_SCHEMA = "lucent.fixture-manifest"
MANIFEST_SCHEMA_VERSION = 1
GENERATOR_NAME = "rawpy-reference"

REPO_ROOT = Path(__file__).resolve().parents[2]

# rawpy raises one exception class per LibRaw error code; invert its table to recover the code.
# LibRawDataError is also raised when LibRaw::error_count() > 0 after an otherwise successful call.
_CODE_BY_ERROR_CLASS = {cls: code for code, cls in rawpy._rawpy._LIBRAW_ERROR_MAP.items()}  # noqa: SLF001


class ProbeInputError(RuntimeError):
    """An input could not be probed (missing file, manifest hash mismatch)."""


def generator_info() -> dict[str, Any]:
    """Identifies the reference decoder build (blueprint D1: record version and flags)."""
    return {
        "name": GENERATOR_NAME,
        "rawpy": rawpy.__version__,
        "libraw": list(rawpy.libraw_version),
        "flags": rawpy.flags,
        "numpy": np.__version__,
        "python": platform.python_version(),
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def json_number(value: float) -> float | str:
    """Returns a JSON-safe float: non-finite values become "nan", "inf" or "-inf"."""
    value = float(value)
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    return value


def _float_list(values: Iterable[float]) -> list[float | str]:
    return [json_number(v) for v in values]


def _float_matrix(matrix: np.ndarray) -> list[list[float | str]]:
    return [_float_list(row) for row in matrix.tolist()]


def raw_buffer_sha256(raw_image: np.ndarray) -> str:
    """SHA-256 of a flat raw buffer as uint16 little-endian samples, row-major, without padding."""
    if raw_image.ndim != 2:
        raise ValueError(f"raw_data_sha256 is defined for flat (2-D) raws only, got shape {raw_image.shape}")
    samples = np.ascontiguousarray(raw_image, dtype="<u2")
    return hashlib.sha256(samples.data).hexdigest()


def build_parity(raw: Any, decode: bool) -> dict[str, Any]:
    """Reads the parity block from an unpacked rawpy.RawPy (or an object with the same properties)."""
    raw_type = raw.raw_type.name.lower()  # forces unpack first, as rawpy's own properties do
    sizes = raw.sizes
    pattern = raw.raw_pattern
    camera_white = raw.camera_white_level_per_channel
    color_desc = raw.color_desc
    if isinstance(color_desc, bytes):
        color_desc = color_desc.decode("ascii")

    parity: dict[str, Any] = {
        "raw_width": int(sizes.raw_width),
        "raw_height": int(sizes.raw_height),
        "width": int(sizes.width),
        "height": int(sizes.height),
        "top_margin": int(sizes.top_margin),
        "left_margin": int(sizes.left_margin),
        "iwidth": int(sizes.iwidth),
        "iheight": int(sizes.iheight),
        "flip": int(sizes.flip),
        "num_colors": int(raw.num_colors),
        "color_desc": color_desc,
        "raw_pattern": None if pattern is None else [[int(v) for v in row] for row in pattern.tolist()],
        "black_level_per_channel": [int(v) for v in raw.black_level_per_channel],
        "white_level": int(raw.white_level),
        "camera_white_level_per_channel": None if camera_white is None else [int(v) for v in camera_white],
        "camera_whitebalance": _float_list(raw.camera_whitebalance),
        "daylight_whitebalance": _float_list(raw.daylight_whitebalance),
        "rgb_xyz_matrix": _float_matrix(np.asarray(raw.rgb_xyz_matrix)),
        "color_matrix": _float_matrix(np.asarray(raw.color_matrix)),
        "raw_type": raw_type,
    }
    if decode and raw_type == "flat":
        parity["raw_data_sha256"] = raw_buffer_sha256(np.asarray(raw.raw_image))
    return parity


def _best_effort(section: str, build: Any) -> Any:
    try:
        return build()
    except Exception as exc:  # noqa: BLE001 - reference extras must never abort a probe
        return {"unavailable": f"{type(exc).__name__}: {exc}", "section": section}


def build_reference_extras(raw: Any) -> dict[str, Any]:
    """rawpy-only diagnostics kept in the full report; not part of the parity contract."""

    def sizes_extra() -> dict[str, Any]:
        s = raw.sizes
        return {
            "pixel_aspect": json_number(s.pixel_aspect),
            "crop_left_margin": int(s.crop_left_margin),
            "crop_top_margin": int(s.crop_top_margin),
            "crop_width": int(s.crop_width),
            "crop_height": int(s.crop_height),
        }

    def raw_image() -> dict[str, Any]:
        full = np.asarray(raw.raw_image)
        visible = np.asarray(raw.raw_image_visible)
        return {
            "shape": list(full.shape),
            "dtype": str(full.dtype),
            "visible_min": int(visible.min()) if visible.size else None,
            "visible_max": int(visible.max()) if visible.size else None,
        }

    def lens() -> dict[str, Any]:
        info = raw.lens
        return {
            "make": info.make,
            "model": info.model,
            "min_focal": json_number(info.min_focal),
            "max_focal": json_number(info.max_focal),
        }

    def other() -> dict[str, Any]:
        info = raw.other
        return {
            "iso_speed": json_number(info.iso_speed),
            "shutter_speed": json_number(info.shutter_speed),
            "aperture": json_number(info.aperture),
            "focal_length": json_number(info.focal_length),
            # rawpy converts to a local-time datetime; the epoch value is machine independent.
            "timestamp_epoch": int(info.timestamp.timestamp()),
            "shot_order": int(info.shot_order),
            "artist": info.artist,
        }

    return {
        "sizes_extra": _best_effort("sizes_extra", sizes_extra),
        "raw_image": _best_effort("raw_image", raw_image),
        "lens": _best_effort("lens", lens),
        "other": _best_effort("other", other),
    }


def _error_record(stage: str, exc: BaseException) -> dict[str, Any]:
    code = None
    for cls in type(exc).__mro__:
        if cls in _CODE_BY_ERROR_CLASS:
            code = _CODE_BY_ERROR_CLASS[cls]
            break
    # rawpy passes LibRaw's strerror() text as bytes.
    message = exc.args[0] if len(exc.args) == 1 else str(exc)
    if isinstance(message, bytes):
        message = message.decode("utf-8", errors="replace")
    return {"stage": stage, "type": type(exc).__name__, "libraw_code": code, "message": str(message)}


def probe_file(
    path: Path,
    *,
    decode: bool,
    shot_select: int = 0,
    fixture_id: str | None = None,
    expected_sha256: str | None = None,
) -> dict[str, Any]:
    """Probes one file and returns the full report. Raises ProbeInputError for unusable inputs."""
    if not path.is_file():
        raise ProbeInputError(f"input file not found: {path}")
    file_sha256 = sha256_file(path)
    if expected_sha256 is not None and file_sha256 != expected_sha256:
        raise ProbeInputError(
            f"{path}: SHA-256 {file_sha256} does not match the manifest ({expected_sha256}); "
            "re-run tools/fetch-fixtures.ps1"
        )

    report: dict[str, Any] = {"schema": SCHEMA, "schema_version": SCHEMA_VERSION}
    if fixture_id is not None:
        report["fixture_id"] = fixture_id
    report.update(
        {
            "file_name": path.name,
            "file_size": path.stat().st_size,
            "file_sha256": file_sha256,
            "generator": generator_info(),
            "options": {"shot_select": shot_select, "decode": decode},
        }
    )

    stage = "open"
    try:
        # rawpy.imread opens the file (wide-character path on Windows) without unpacking.
        with rawpy.imread(str(path), shot_select=shot_select) as raw:
            stage = "unpack"
            raw.unpack()
            stage = "read"
            parity = build_parity(raw, decode)
            extras = build_reference_extras(raw)
    except (rawpy.LibRawError, NotImplementedError) as exc:
        # NotImplementedError is rawpy's documented answer for CFA filter values it cannot describe.
        report.update({"status": "error", "error": _error_record(stage, exc), "parity": None})
        return report

    report.update({"status": "ok", "parity": parity, "reference": extras})
    return report


def parity_reference(report: dict[str, Any]) -> dict[str, Any]:
    """The committed, compact form of a report: everything except the rawpy-only extras."""
    return {key: value for key, value in report.items() if key != "reference"}


def _format(value: Any, depth: int) -> str:
    """JSON with 2-space indentation, keeping lists of scalars (matrix rows) on one line."""
    pad = "  " * depth
    if isinstance(value, dict):
        if not value:
            return "{}"
        items = [f'{pad}  {json.dumps(str(k), ensure_ascii=False)}: {_format(v, depth + 1)}' for k, v in value.items()]
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"
    if isinstance(value, (list, tuple)):
        if all(not isinstance(v, (dict, list, tuple)) for v in value):
            return "[" + ", ".join(_format(v, depth + 1) for v in value) + "]"
        return "[\n" + ",\n".join(f"{pad}  {_format(v, depth + 1)}" for v in value) + "\n" + pad + "]"
    return json.dumps(value, ensure_ascii=False, allow_nan=False)


def dumps(document: Any) -> str:
    """Deterministic, diff-friendly JSON text (trailing newline included)."""
    return _format(document, 0) + "\n"


def write_atomically(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        os.replace(temp_name, path)
    except BaseException:
        Path(temp_name).unlink(missing_ok=True)
        raise


def load_manifest_inputs(
    manifest_path: Path, tier: str, ids: Sequence[str]
) -> list[tuple[str, Path, str | None]]:
    """Returns (fixture id, cached path, expected sha256) for the selected manifest entries."""
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    if document.get("schema") != MANIFEST_SCHEMA:
        raise ValueError(f"{manifest_path}: schema is not {MANIFEST_SCHEMA!r}")
    version = document.get("schema_version")
    if not isinstance(version, int) or version < 1 or version > MANIFEST_SCHEMA_VERSION:
        raise ValueError(f"{manifest_path}: unsupported schema_version {version!r}")
    entries = document.get("fixtures") or []
    known = {entry["id"] for entry in entries}
    unknown = [i for i in ids if i not in known]
    if unknown:
        raise ValueError(f"unknown fixture id(s): {', '.join(unknown)}")
    cache = manifest_path.parent / "cache"
    selected = []
    for entry in entries:
        if ids:
            if entry["id"] not in ids:
                continue
        elif tier != "all" and entry.get("tier") != tier:
            continue
        path = cache / entry["id"] / entry["file_name"]
        selected.append((entry["id"], path, entry.get("sha256")))
    return selected


def _summary(name: str, report: dict[str, Any]) -> str:
    if report["status"] != "ok":
        error = report["error"]
        return f"{name}: LibRaw error at {error['stage']}: {error['type']}: {error['message']}"
    p = report["parity"]
    return (
        f"{name}: ok {p['raw_type']} {p['color_desc']} colors={p['num_colors']} "
        f"raw={p['raw_width']}x{p['raw_height']} visible={p['width']}x{p['height']} flip={p['flip']}"
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Write rawpy reference probe reports (lucent.probe-report v1).",
        epilog="See the module docstring and research/README.md for the field contract.",
    )
    parser.add_argument("files", nargs="*", type=Path, help="RAW files to probe")
    parser.add_argument("--decode", action="store_true", help="add raw_data_sha256 (flat raws)")
    parser.add_argument("--shot-select", type=int, default=0, help="LibRaw shot_select (default 0)")
    parser.add_argument("--manifest", type=Path, help="probe cached fixtures listed in this manifest")
    parser.add_argument("--tier", choices=("smoke", "qualification", "all"), default="all")
    parser.add_argument("--id", action="append", default=[], help="fixture id (repeatable, comma-separated)")
    parser.add_argument("--out-dir", type=Path, help="write <name>.json full reports here")
    parser.add_argument("--parity-dir", type=Path, help="write <name>.parity.json compact references here")
    args = parser.parse_args(argv)

    if bool(args.files) == bool(args.manifest):
        parser.error("give either RAW files or --manifest")
    if args.shot_select < 0:
        parser.error("--shot-select must be >= 0")
    ids = [part.strip() for value in args.id for part in value.split(",") if part.strip()]

    try:
        if args.manifest:
            inputs = load_manifest_inputs(args.manifest, args.tier, ids)
        else:
            inputs = [(path.stem, path, None) for path in args.files]
    except (OSError, ValueError, KeyError) as exc:
        print(f"probe_reference: error: {exc}", file=sys.stderr)
        return 2

    reports: list[dict[str, Any]] = []
    failures = 0
    for name, path, expected in inputs:
        try:
            report = probe_file(
                path,
                decode=args.decode,
                shot_select=args.shot_select,
                fixture_id=name if args.manifest else None,
                expected_sha256=expected,
            )
        except (ProbeInputError, OSError) as exc:
            failures += 1
            print(f"{name}: FAILED: {exc}", file=sys.stderr)
            continue
        except Exception as exc:  # noqa: BLE001 - report and continue with the remaining inputs
            failures += 1
            print(f"{name}: FAILED: unexpected {type(exc).__name__}: {exc}", file=sys.stderr)
            continue
        print(_summary(name, report), file=sys.stderr)
        if args.out_dir:
            write_atomically(args.out_dir / f"{name}.json", dumps(report))
        if args.parity_dir:
            write_atomically(args.parity_dir / f"{name}.parity.json", dumps(parity_reference(report)))
        if not args.out_dir and not args.parity_dir:
            reports.append(report)

    if reports:
        sys.stdout.write(dumps(reports[0] if len(reports) == 1 else reports))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
