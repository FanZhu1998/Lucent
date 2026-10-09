"""Reproduces Image-Adaptive-3DLUT's sRGB demo on CPU and records hashes. Research only.

Blueprint Appendix D2, steps 2-4. Runs the repository's own ``demo_eval.py`` (pinned checkout,
unmodified on disk) with the two substitutions its README documents for PyTorch 1.x, applied to a
copy under research/lut/build/demo:

    from models import *                ->  from models_x import *
    result = trilinear_(LUT, img)       ->  _, result = trilinear_(LUT, img)

plus ``map_location="cpu"`` on its two ``torch.load`` calls, because the checkpoints hold CUDA
storages and cannot otherwise be loaded without CUDA (CPU_SUBSTITUTIONS; not in the README).
The XYZ-only imports ``cv2`` and ``torchvision_x_functional`` are replaced by stubs that raise on use.

The script runs in this process (runpy) with the working directory set to the checkout, as the
historical command ``python demo_eval.py --input_color_space sRGB`` expects. ``models_x`` imports
``trilinear``; three interchangeable backends are installed under that name in turn:

    extension  the repository's CPU extension, trilinear_cpp/src/trilinear.cpp, built unmodified
               with MSVC (build_trilinear_ext.ps1)
    torch      trilinear_torch.ExtensionApi, pure PyTorch with the extension's arithmetic
    numpy      trilinear_numpy.forward, an independent NumPy mirror of the C++ loop

Each backend call is recorded, so the float32 output is captured before demo_eval quantises it in
place. The three outputs must be bit-identical. Further analyses (all on the same input):
legacy binsize (models.py), thread determinism, an independent NumPy predictor, sensitivity to the
resolution of the image given to the classifier, and LUT diagnostics.

Outputs: research/lut/results/demo_reproduction.json, the synthetic-chart preview and LUT curve PNGs
in research/lut/results/, and photo previews in research/lut/build/previews/ (not committed: the
demo photographs are MIT-Adobe FiveK images under research-only licences; see the README).

Usage (repository root):  research\\lut\\.venv\\Scripts\\python.exe research\\lut\\tools\\reproduce_demo.py
"""

from __future__ import annotations

import argparse
import colorsys
import datetime as dt
import difflib
import io
import os
import runpy
import shutil
import sys
import time
import types
import warnings
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lut_common as lc  # noqa: E402  (sets sys.dont_write_bytecode before upstream imports)

import numpy as np  # noqa: E402
import torch  # noqa: E402
from PIL import Image, ImageCms, ImageDraw  # noqa: E402

import classifier_numpy as cnp  # noqa: E402
import trilinear_numpy as tnp  # noqa: E402
import trilinear_torch as tt  # noqa: E402

README_SUBSTITUTIONS = [
    ("from models import *", "from models_x import *"),
    ("result = trilinear_(LUT, img)", "_, result = trilinear_(LUT, img)"),
]
# Not in the README: the sRGB checkpoints hold CUDA storages ('cuda:0') and torch.load restores
# storages to the device they were saved from, so without CUDA it needs map_location. Nothing
# else changes.
CPU_SUBSTITUTIONS = [
    ('LUTs = torch.load("%s/LUTs.pth" % opt.model_dir)',
     'LUTs = torch.load("%s/LUTs.pth" % opt.model_dir, map_location="cpu")'),
    ('classifier.load_state_dict(torch.load("%s/classifier.pth" % opt.model_dir))',
     'classifier.load_state_dict(torch.load("%s/classifier.pth" % opt.model_dir, map_location="cpu"))'),
]
DEMO_IMAGE = ("demo_images/sRGB", "a1629.jpg")
EXTRA_IMAGES = [("local_tone_mapping", "a1509.jpg")]  # bundled sRGB FiveK image, not part of the demo
FIVEK = {
    "a1629.jpg": {"fivek_id": "a1629-JI2E5331", "licence_file": "LicenseAdobeMIT.txt"},
    "a1509.jpg": {"fivek_id": "a1509-dvf_034", "licence_file": "LicenseAdobe.txt"},
}
DIM = 33


# ---------------------------------------------------------------------------- backends


class NumpyBackend(types.ModuleType):
    """trilinear_numpy.forward behind the extension's module API."""

    def __init__(self) -> None:
        super().__init__("trilinear")

    @staticmethod
    def forward(lut, image, output, lut_dim, shift, binsize, width, height, batch) -> int:
        if image.dim() != 4 or image.shape[1] != 3:
            return 0
        if batch != 1:
            raise NotImplementedError("batch > 1")
        out = tnp.forward(lut.detach().numpy(), image[0].detach().numpy(), binsize)
        with torch.no_grad():
            output[0].copy_(torch.from_numpy(out))
        return 1

    @staticmethod
    def backward(*args: Any) -> int:
        raise NotImplementedError("inference only")


class Recording(types.ModuleType):
    """Wraps a backend and keeps a copy of every forward call's tensors."""

    def __init__(self, inner: Any) -> None:
        super().__init__("trilinear")
        self.inner = inner
        self.calls: list[dict[str, Any]] = []

    def forward(self, lut, image, output, lut_dim, shift, binsize, width, height, batch) -> int:
        rc = self.inner.forward(lut, image, output, lut_dim, shift, binsize, width, height, batch)
        self.calls.append({
            "lut": lut.detach().clone(), "image": image.detach().clone(), "output": output.detach().clone(),
            "lut_dim": lut_dim, "shift": shift, "binsize": binsize, "width": width, "height": height,
            "batch": batch, "rc": rc,
        })
        return rc

    def backward(self, *args: Any) -> int:
        return self.inner.backward(*args)


def backends(ext: types.ModuleType | None) -> dict[str, Any]:
    found: dict[str, Any] = {}
    if ext is not None:
        found["extension"] = ext
    found["torch"] = tt.ExtensionApi()
    found["numpy"] = NumpyBackend()
    return found


def apply_lut(ext: types.ModuleType | None, lut: torch.Tensor, img: torch.Tensor, binsize: float) -> torch.Tensor:
    """One forward pass through the extension (or the PyTorch sampler when it is not built)."""
    img = img.contiguous()
    if ext is None:
        return tt.forward(lut, img, binsize)
    out = torch.empty_like(img)
    assert ext.forward(lut.contiguous(), img, out, DIM, DIM ** 3, binsize, img.shape[2], img.shape[3], 1) == 1
    return out


def quantise(out: torch.Tensor) -> np.ndarray:
    """demo_eval.py's own conversion: mul(255).add(0.5).clamp(0, 255) then uint8 (truncation)."""
    return out.squeeze(0).clone().mul_(255).add_(0.5).clamp_(0, 255).permute(1, 2, 0).to(torch.uint8).numpy()


# ---------------------------------------------------------------------------- demo runs


def patched_demo(checkout: Path, dest: Path) -> dict[str, Any]:
    original = (checkout / "demo_eval.py").read_bytes()
    text = original.decode("utf-8")
    lines = text.split("\n")
    for old, new in README_SUBSTITUTIONS + CPU_SUBSTITUTIONS:
        hits = [i for i, line in enumerate(lines) if line.rstrip("\r") == old]
        if len(hits) != 1:
            raise SystemExit(f"demo_eval.py: expected exactly one line {old!r}, found {len(hits)}")
        cr = "\r" if lines[hits[0]].endswith("\r") else ""
        lines[hits[0]] = new + cr
    patched = "\n".join(lines).encode("utf-8")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(patched)
    diff = list(difflib.unified_diff(text.splitlines(), patched.decode().splitlines(), "demo_eval.py",
                                     "demo_eval.py (README substitutions)", lineterm="", n=0))
    return {"original_sha256": lc.sha256_bytes(original), "patched_sha256": lc.sha256_bytes(patched), "diff": diff}


def run_demo(checkout: Path, script: Path, image_dir: str, image_name: str, backend: Any, out_dir: Path) -> dict[str, Any]:
    for name in ("models_x", "trilinear"):
        sys.modules.pop(name, None)
    stubs = lc.install_xyz_stubs()
    rec = Recording(backend)
    sys.modules["trilinear"] = rec
    argv = ["demo_eval.py", "--image_dir", image_dir, "--image_name", image_name,
            "--input_color_space", "sRGB", "--model_dir", "pretrained_models", "--output_dir", str(out_dir)]
    saved_argv, saved_cwd, saved_path = sys.argv, os.getcwd(), list(sys.path)
    try:
        sys.argv = argv
        os.chdir(checkout)
        sys.path.insert(0, str(checkout))
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            t0 = time.perf_counter()
            g = runpy.run_path(str(script), run_name="__main__")
            seconds = time.perf_counter() - t0
    finally:
        sys.argv = saved_argv
        os.chdir(saved_cwd)
        sys.path[:] = saved_path
        for name in ("models_x", "trilinear", *stubs):
            sys.modules.pop(name, None)
    if len(rec.calls) != 1:
        raise RuntimeError(f"expected one trilinear call, got {len(rec.calls)}")
    call = rec.calls[0]
    if not (torch.equal(call["lut"], g["LUT"].detach()) and torch.equal(call["image"], g["img"])):
        raise RuntimeError("the recorded call does not match demo_eval's LUT and image")
    if not np.array_equal(quantise(call["output"]), g["ndarr"]):
        raise RuntimeError("demo_eval's uint8 array differs from quantising the recorded float output")
    return {
        "globals": g, "call": call, "seconds": seconds,
        "warnings": sorted({f"{w.category.__name__}: {str(w.message).splitlines()[0][:160]} "
                            f"({Path(w.filename).name})" for w in caught}),
        "argv": argv[1:],
    }


def lut_diagnostics(lut: np.ndarray) -> dict[str, Any]:
    """lut: (3, D, D, D) [c][b][g][r]. Ranges, monotonicity along each channel's own axis and the
    neutral diagonal, distance from identity. Diagnostics, not pass/fail criteria."""
    d = lut.shape[-1]
    grid = np.arange(d, dtype=np.float64) / (d - 1)
    ident = np.stack([
        np.broadcast_to(grid[None, None, :], (d, d, d)),  # R varies along r (last axis)
        np.broadcast_to(grid[None, :, None], (d, d, d)),  # G along g
        np.broadcast_to(grid[:, None, None], (d, d, d)),  # B along b
    ])
    own_axis_steps = [np.diff(lut[0], axis=2), np.diff(lut[1], axis=1), np.diff(lut[2], axis=0)]
    neutral = np.stack([lut[c, np.arange(d), np.arange(d), np.arange(d)] for c in range(3)])
    return {
        "min_per_channel": [float(lut[c].min()) for c in range(3)],
        "max_per_channel": [float(lut[c].max()) for c in range(3)],
        "fraction_below_0": float((lut < 0).mean()),
        "fraction_above_1": float((lut > 1).mean()),
        "own_axis_decreasing_steps": [int((s < 0).sum()) for s in own_axis_steps],
        "own_axis_steps_total": int(own_axis_steps[0].size),
        "max_abs_deviation_from_identity": float(np.abs(lut - ident).max()),
        "neutral_axis_nondecreasing": [bool((np.diff(neutral[c]) >= 0).all()) for c in range(3)],
        "neutral_axis_max_channel_spread": float((neutral.max(axis=0) - neutral.min(axis=0)).max()),
        "neutral_axis": [[round(float(v), 6) for v in neutral[c]] for c in range(3)],
    }


def image_record(path: Path, extra: dict[str, Any]) -> dict[str, Any]:
    from PIL import JpegImagePlugin

    im = Image.open(path)
    ex = im.getexif()
    icc = im.info.get("icc_profile", b"")
    desc = ImageCms.getProfileDescription(ImageCms.ImageCmsProfile(io.BytesIO(icc))).strip() if icc else None
    decoded = np.asarray(im)
    sampling = JpegImagePlugin.get_sampling(im) if im.format == "JPEG" else None
    return {
        "file": path.name, "file_sha256": lc.sha256_file(path), "bytes": path.stat().st_size,
        "format": im.format, "mode": im.mode, "size_wh": list(im.size),
        "jpeg_subsampling": {0: "4:4:4", 1: "4:2:2", 2: "4:2:0"}.get(sampling, str(sampling)),
        "jpeg_progressive": bool(im.info.get("progressive") or im.info.get("progression")),
        "icc_profile": desc, "exif_make_model": [ex.get(271), ex.get(272)], "exif_software": ex.get(305),
        "exif_orientation": ex.get(274), "decoded_uint8": lc.array_record(decoded),
        "decoded_value_range": [int(decoded.min()), int(decoded.max())], **extra,
    }


def checkpoint_record(checkout: Path) -> dict[str, Any]:
    import zipfile

    rec: dict[str, Any] = {}
    for name in ("LUTs.pth", "classifier.pth"):
        p = checkout / "pretrained_models" / "sRGB" / name
        locations: list[str] = []

        def to_cpu(storage, location, _seen=locations):
            _seen.append(location)
            return storage

        sd = torch.load(p, map_location=to_cpu, weights_only=True)
        flat = {f"{k}.{kk}": vv for k, v in sd.items() for kk, vv in v.items()} if name == "LUTs.pth" else sd
        rec[name] = {
            "file_sha256": lc.sha256_file(p), "bytes": p.stat().st_size,
            "serialization": "zip" if zipfile.is_zipfile(p) else "legacy pickle (PyTorch < 1.6)",
            "saved_storage_locations": sorted(set(locations)),
            "loaded_with": "torch.load(map_location=cpu, weights_only=True)",
            "git_added": lc.git(checkout, "log", "--diff-filter=A", "--format=%h %cI %s", "--",
                                f"pretrained_models/sRGB/{name}"),
            "state": lc.state_dict_record(flat),
        }
    return rec


def thread_determinism(ext, g: dict[str, Any], img: torch.Tensor, out_ref: torch.Tensor) -> dict[str, Any]:
    """PyTorch's CPU convolutions (oneDNN) may split reductions differently by thread count. Repeat
    runs per thread count, compare the weights with the demo's (default thread count), and show what
    the 1-thread weights do to the output."""
    default = torch.get_num_threads()
    per: dict[int, list[torch.Tensor]] = {}
    try:
        for n in sorted({1, 2, default}):
            torch.set_num_threads(n)
            with torch.no_grad():
                per[n] = [g["classifier"](img).squeeze().clone() for _ in range(3)]
    finally:
        torch.set_num_threads(default)
    ref = per[default][0]
    p1 = per[1][0]
    lut1 = p1[0] * g["LUT0"].LUT + p1[1] * g["LUT1"].LUT + p1[2] * g["LUT2"].LUT
    return {
        "default_threads": default,
        "per_thread_count": {
            str(n): {
                "repeat_runs_bit_identical": all(torch.equal(r, runs[0]) for r in runs),
                "weights_bit_identical_to_default": torch.equal(runs[0], ref),
                "max_abs_weight_difference_vs_default": float(max((r - ref).abs().max() for r in runs)),
            }
            for n, runs in per.items()
        },
        "output_effect_of_1_thread_weights": compare_outputs(ext, lut1.detach(), out_ref, img, lc.BINSIZE_MODELS_X(DIM)),
    }


def compare_outputs(ext, lut_a: torch.Tensor, out_ref: torch.Tensor, img: torch.Tensor, binsize: float) -> dict[str, Any]:
    out = apply_lut(ext, lut_a, img, binsize)
    qa, qb = quantise(out).astype(np.int16), quantise(out_ref).astype(np.int16)
    dq = np.abs(qa - qb)
    return {
        "max_abs_float_difference": float((out - out_ref).abs().max()),
        "mean_abs_float_difference": float((out - out_ref).abs().mean()),
        "uint8_samples_differing": int((dq > 0).sum()),
        "uint8_samples_differing_by_more_than_1": int((dq > 1).sum()),
        "uint8_max_difference": int(dq.max()),
        "uint8_samples_total": int(dq.size),
    }


def resolution_sensitivity(ext, g: dict[str, Any], src: Path, out_ref: torch.Tensor) -> list[dict[str, Any]]:
    """The classifier resizes whatever it receives to 256x256 by bilinear interpolation without
    antialiasing, so the LUT depends on the resolution of the image it is given. The LUT predicted
    from each resized copy (PIL BOX filter, then to_tensor) is applied to the full-resolution input
    and compared with the demo's output."""
    import torchvision.transforms.functional as TF

    im = Image.open(src)
    full = g["img"]
    rows = []
    w0, h0 = im.size
    for w, h in [(w0, h0), (1080, 720), (720, 480), (540, 360), (360, 240)]:
        small = im if (w, h) == (w0, h0) else im.resize((w, h), Image.Resampling.BOX)
        x = TF.to_tensor(small).unsqueeze(0)
        with torch.no_grad():
            pred = g["classifier"](x).squeeze()
            lut = pred[0] * g["LUT0"].LUT + pred[1] * g["LUT1"].LUT + pred[2] * g["LUT2"].LUT
        row = {"classifier_input_wh": [w, h], "weights": [round(float(v), 6) for v in pred]}
        row.update(compare_outputs(ext, lut, out_ref, full, lc.BINSIZE_MODELS_X(DIM)))
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------- previews


def synthetic_chart(width: int = 384) -> np.ndarray:
    """A rights-clean 8-bit sRGB test chart: hue x value sweep at full saturation, a saturation
    sweep at mid value, then neutral, red, green and blue ramps."""
    rows = []
    hues = np.linspace(0.0, 1.0, width, endpoint=False)
    for v in np.linspace(1.0, 0.05, 96):
        rows.append([colorsys.hsv_to_rgb(h, 1.0, v) for h in hues])
    for s in np.linspace(0.0, 1.0, 32):
        rows.append([colorsys.hsv_to_rgb(h, s, 0.6) for h in hues])
    ramp = np.linspace(0.0, 1.0, width)
    for color in [(1, 1, 1), (1, 0, 0), (0, 1, 0), (0, 0, 1)]:
        for _ in range(32):
            rows.append([(t * color[0], t * color[1], t * color[2]) for t in ramp])
    return np.clip(np.round(np.asarray(rows) * 255.0), 0, 255).astype(np.uint8)


def write_previews(ext, lut: torch.Tensor, demo_input: np.ndarray, demo_output: np.ndarray, tag: str) -> dict[str, Any]:
    import torchvision.transforms.functional as TF

    lc.RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    chart = synthetic_chart()
    chart_out = quantise(apply_lut(ext, lut, TF.to_tensor(Image.fromarray(chart)).unsqueeze(0), lc.BINSIZE_MODELS_X(DIM)))
    gap = np.full((chart.shape[0], 8, 3), 255, np.uint8)
    chart_path = lc.RESULTS_DIR / f"{tag}_lut_on_synthetic_chart.png"
    Image.fromarray(np.concatenate([chart, gap, chart_out], axis=1)).save(chart_path, optimize=True)

    curves_path = lc.RESULTS_DIR / f"{tag}_lut_neutral_curves.png"
    width, height, left, right, top, bottom = 360, 380, 40, 14, 34, 40
    y_lo, y_hi = -0.2, 1.4
    plot_w, plot_h = width - left - right, height - top - bottom

    def px(x: float, y: float) -> tuple[float, float]:
        return left + plot_w * x, top + plot_h * (y_hi - y) / (y_hi - y_lo)

    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(canvas)
    for y in (0.0, 0.25, 0.5, 0.75, 1.0):
        draw.line([px(0, y), px(1, y)], fill=(150, 150, 150) if y in (0.0, 1.0) else (228, 228, 228))
        draw.text((4, px(0, y)[1] - 6), f"{y:.2f}", fill=(80, 80, 80))
    for x in (0.25, 0.5, 0.75):
        draw.line([px(x, y_lo), px(x, y_hi)], fill=(228, 228, 228))
    draw.rectangle([px(0, y_hi), px(1, y_lo)], outline=(0, 0, 0))
    draw.line([px(0, 0), px(1, 1)], fill=(170, 170, 170))
    lut_np = lut.detach().numpy()
    d = lut_np.shape[-1]
    idx = np.arange(d)
    for c, col in enumerate([(200, 30, 30), (20, 150, 40), (30, 60, 200)]):
        ys = lut_np[c, idx, idx, idx]
        draw.line([px(k / (d - 1), float(np.clip(v, y_lo, y_hi))) for k, v in enumerate(ys)], fill=col, width=2)
    draw.text((left, 8), f"{tag}: fused LUT nodes on the neutral axis", fill=(0, 0, 0))
    draw.text((left, 20), "R, G, B; grey lines: output 0 and 1, identity", fill=(90, 90, 90))
    draw.text((left, height - bottom + 8), "input k/32 on all three channels, 0 .. 1", fill=(0, 0, 0))
    canvas.save(curves_path, optimize=True)

    preview_dir = lc.BUILD_DIR / "previews"
    preview_dir.mkdir(parents=True, exist_ok=True)
    photo_path = preview_dir / f"{tag}_input_vs_output.png"
    a = Image.fromarray(demo_input).reduce(4)
    b = Image.fromarray(demo_output).reduce(4)
    pair = Image.new("RGB", (a.width * 2 + 8, a.height), (255, 255, 255))
    pair.paste(a, (0, 0))
    pair.paste(b, (a.width + 8, 0))
    pair.save(photo_path, optimize=True)
    return {
        "committed": {p.name: {"sha256": lc.sha256_file(p), "bytes": p.stat().st_size} for p in (chart_path, curves_path)},
        "not_committed": str(photo_path.relative_to(lc.REPO_ROOT)).replace("\\", "/"),
        "chart_input_uint8": lc.array_record(chart), "chart_output_uint8": lc.array_record(chart_out),
    }


# ---------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=lc.RESULTS_DIR / "demo_reproduction.json")
    args = ap.parse_args()

    checkout = lc.checkout_path()
    upstream = lc.verify_checkout(checkout)
    ext = lc.load_extension()
    if ext is None:
        print("warning: the CPU extension is not built (build_trilinear_ext.ps1); parity with it is not shown")
    work = lc.BUILD_DIR / "demo"
    if work.exists():
        shutil.rmtree(work)
    script = work / "demo_eval_models_x.py"
    patch = patched_demo(checkout, script)

    staged = work / "inputs" / "sRGB"
    staged.mkdir(parents=True)
    images = [DEMO_IMAGE] + EXTRA_IMAGES
    for folder, name in EXTRA_IMAGES:
        shutil.copy2(checkout / folder / name, staged / name)

    report: dict[str, Any] = {
        "schema": "lucent.lut-research.demo-reproduction",
        "schema_version": 1,
        "run_date": dt.date.today().isoformat(),
        "purpose": "Blueprint Appendix D2 steps 2-4: reproduce the sRGB demo of Image-Adaptive-3DLUT on CPU.",
        "upstream": upstream,
        "environment": lc.environment(),
        "extension_build": lc.extension_build_record(ext),
        "command": {
            "historical": "python demo_eval.py --input_color_space sRGB",
            "run_as": "runpy of a copy of demo_eval.py with the README's two PyTorch 1.x substitutions and "
                      "map_location='cpu' on its two torch.load calls; working directory = the checkout",
            "readme_substitutions": [list(s) for s in README_SUBSTITUTIONS],
            "cpu_only_substitutions": [list(s) for s in CPU_SUBSTITUTIONS],
            "demo_eval_patch": patch,
            "xyz_only_imports_stubbed": ["cv2", "torchvision_x_functional"],
        },
        "checkpoints": checkpoint_record(checkout),
        "identity_lut_txt_sha256": lc.sha256_file(checkout / "IdentityLUT33.txt"),
        "images": {},
    }

    for folder, name in images:
        is_demo = (folder, name) == DEMO_IMAGE
        image_dir = DEMO_IMAGE[0].split("/")[0] if is_demo else str(staged.parent)
        src = checkout / folder / name
        entry: dict[str, Any] = {
            "role": "the bundled sRGB demo input (demo_eval.py default)" if is_demo
            else "another bundled sRGB FiveK image (local_tone_mapping/, used by upstream's MATLAB demo), "
                 "run through the same demo for a second example",
            "input": image_record(src, FIVEK[name]),
            "backends": {},
        }
        runs = {}
        for bname, backend in backends(ext).items():
            r = run_demo(checkout, script, image_dir, name, backend, work / "out" / bname / Path(name).stem)
            runs[bname] = r
            jpg = work / "out" / bname / Path(name).stem / "result.jpg"
            entry["backends"][bname] = {
                "output_float32": lc.array_record(r["call"]["output"]),
                "output_uint8": lc.array_record(r["globals"]["ndarr"]),
                "result_jpg_sha256 (encoder-dependent, informational)": lc.sha256_file(jpg),
                "seconds_whole_demo": round(r["seconds"], 3),
            }
            lc.assert_checkout_unchanged(checkout)
        first = next(iter(runs.values()))
        out_hashes = {v["output_float32"]["sha256"] for v in entry["backends"].values()}
        entry["all_backends_bit_identical"] = len(out_hashes) == 1
        g, call = first["globals"], first["call"]
        img, lut_t = g["img"], g["LUT"].detach()
        with torch.no_grad():
            pred = g["classifier"](img).squeeze()
            refused = pred[0] * g["LUT0"].LUT + pred[1] * g["LUT1"].LUT + pred[2] * g["LUT2"].LUT
        entry["demo"] = {
            "argv": first["argv"],
            "runtime_warnings": first["warnings"],
            "input_tensor": lc.array_record(img),
            "binsize_passed": call["binsize"], "binsize_float32_bits": lc.float32_bits(call["binsize"]),
            "weights": [float(v) for v in pred], "weights_float32_bits": [lc.float32_bits(float(v)) for v in pred],
            "weights_sum": float(pred.sum()),
            "recomputed_fused_lut_bit_identical": bool(torch.equal(refused, lut_t)),
            "fused_lut": lc.array_record(lut_t),
            "output_float32": entry["backends"][next(iter(runs))]["output_float32"],
            "output_uint8": entry["backends"][next(iter(runs))]["output_uint8"],
            "output_fraction_below_0": float((call["output"] < 0).float().mean()),
            "output_fraction_above_1": float((call["output"] > 1).float().mean()),
            "mean_rgb_in": [round(float(v), 5) for v in img[0].mean(dim=(1, 2))],
            "mean_rgb_out": [round(float(v), 5) for v in call["output"][0].mean(dim=(1, 2))],
        }
        entry["lut_diagnostics"] = lut_diagnostics(lut_t.numpy())
        entry["legacy_binsize_models_py"] = {
            "binsize": lc.BINSIZE_MODELS_LEGACY(DIM),
            "note": "models.py (PyTorch 0.4.1 + trilinear_c) passes 1.0001/32; models_x.py passes 1.000001/32. "
                    "Same fused LUT and input, applied with the extension.",
            **compare_outputs(ext, lut_t, call["output"], img, lc.BINSIZE_MODELS_LEGACY(DIM)),
        }
        entry["thread_determinism"] = thread_determinism(ext, g, img, call["output"])

        state = g["classifier"].state_dict()
        pred_np, trace = cnp.forward(state, img[0].numpy())
        lut_np = cnp.fuse_luts(pred_np, [g[k].LUT.detach().numpy() for k in ("LUT0", "LUT1", "LUT2")])
        entry["independent_numpy_predictor"] = {
            "weights_float64": [float(v) for v in pred_np],
            "max_abs_weight_difference_vs_torch": float(np.abs(pred_np - pred.double().numpy()).max()),
            "max_abs_fused_lut_difference_vs_torch": float(np.abs(lut_np - lut_t.numpy()).max()),
            "layer_trace": trace,
            **compare_outputs(ext, torch.from_numpy(lut_np), call["output"], img, lc.BINSIZE_MODELS_X(DIM)),
        }
        entry["classifier_input_resolution_sensitivity"] = resolution_sensitivity(ext, g, src, call["output"])
        tag = Path(name).stem
        entry["previews"] = write_previews(ext, lut_t, np.asarray(Image.open(src)), g["ndarr"], tag)
        report["images"][name] = entry
        print(f"{name}: weights {entry['demo']['weights']} bit-identical backends: {entry['all_backends_bit_identical']} "
              f"({', '.join(entry['backends'])})")

    lc.assert_checkout_unchanged(checkout)
    lc.write_json(args.out, report)
    print(f"wrote {args.out}")
    ok = all(e["all_backends_bit_identical"] for e in report["images"].values())
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
