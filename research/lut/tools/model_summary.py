"""Architecture, parameters, FLOPs and indicative CPU timings of the upstream sRGB model. Research only.

Input to the native-implementation plan (ADR 0019). Counts are analytic (one multiply-accumulate =
one MAC = two FLOPs) and the convolution total is cross-checked with torch.utils.flop_counter.
Timings are single measurements on the research laptop (median of repeats), not benchmarks.

Usage (repository root): research\\lut\\.venv\\Scripts\\python.exe research\\lut\\tools\\model_summary.py
"""

from __future__ import annotations

import argparse
import datetime as dt
import statistics
import sys
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lut_common as lc  # noqa: E402

import numpy as np  # noqa: E402
import torch  # noqa: E402
from torch.utils.flop_counter import FlopCounterMode  # noqa: E402

import classifier_numpy as cnp  # noqa: E402
import trilinear_torch as tt  # noqa: E402
import upstream_model as um  # noqa: E402

DIM = 33


def layer_table(classifier: torch.nn.Module, img: torch.Tensor) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    hooks = []
    for idx, layer in enumerate(classifier.model):
        def hook(mod, inp, out, idx=idx):
            x, y = inp[0], out
            params = sum(p.numel() for p in mod.parameters(recurse=False))
            macs, other = 0, 0
            kind = type(mod).__name__
            if isinstance(mod, torch.nn.Conv2d):
                kh, kw = mod.kernel_size
                macs = y.numel() * (mod.in_channels // mod.groups) * kh * kw
                other = y.numel()  # bias additions
                desc = f"Conv2d({mod.in_channels}, {mod.out_channels}, {kh}x{kw}, stride {mod.stride[0]}, pad {mod.padding[0]})"
            elif isinstance(mod, torch.nn.InstanceNorm2d):
                other = 8 * y.numel()  # mean, variance, normalise, scale, shift (approximate)
                desc = f"InstanceNorm2d({mod.num_features}, affine={mod.affine}, eps={mod.eps}, " \
                       f"track_running_stats={mod.track_running_stats})"
            elif isinstance(mod, torch.nn.LeakyReLU):
                other = y.numel()
                desc = f"LeakyReLU({mod.negative_slope})"
            elif isinstance(mod, torch.nn.Upsample):
                other = 7 * y.numel()  # 4 multiplies, 3 adds per output sample (separable weights shared)
                desc = f"Upsample(size={mod.size}, mode={mod.mode}, align_corners={mod.align_corners} -> False)"
            elif isinstance(mod, torch.nn.Dropout):
                desc = f"Dropout({mod.p}) (identity in eval)"
            else:
                desc = kind
            rows.append({"index": idx, "layer": desc, "input": list(x.shape), "output": list(y.shape),
                         "params": params, "macs": macs, "other_flops_approx": other})
        hooks.append(layer.register_forward_hook(hook))
    try:
        with torch.no_grad():
            classifier(img)
    finally:
        for h in hooks:
            h.remove()
    return rows


def median_seconds(fn, repeats: int) -> float:
    fn()  # warm-up
    times = []
    for _ in range(repeats):
        t0 = time.perf_counter()
        fn()
        times.append(time.perf_counter() - t0)
    return statistics.median(times)


def timings(model: um.UpstreamModel, img: torch.Tensor, lut: torch.Tensor, ext) -> dict[str, Any]:
    out: dict[str, Any] = {"note": "median wall-clock seconds on the research laptop; indicative only"}
    default = torch.get_num_threads()
    small = torch.nn.functional.interpolate(img, size=(256, 256), mode="bilinear", align_corners=False)
    try:
        for n in sorted({1, default}):
            torch.set_num_threads(n)
            with torch.no_grad():
                out[f"classifier_torch_{n}_threads_on_1620x1080"] = round(median_seconds(lambda: model.classifier(img), 10), 4)
                out[f"classifier_torch_{n}_threads_on_256x256"] = round(median_seconds(lambda: model.classifier(small), 10), 4)
    finally:
        torch.set_num_threads(default)
    state = model.classifier.state_dict()
    out["classifier_numpy_float64_unoptimised"] = round(median_seconds(lambda: cnp.forward(state, img[0].numpy()), 3), 3)
    with torch.no_grad():
        out["lut_fusion_torch"] = round(median_seconds(
            lambda: 1.0 * model.luts[0] + 0.5 * model.luts[1] + 0.25 * model.luts[2], 20), 5)
    pixels = img.shape[2] * img.shape[3]
    if ext is not None:
        res = torch.empty_like(img)
        s = median_seconds(lambda: ext.forward(lut, img, res, DIM, DIM ** 3, lc.BINSIZE_MODELS_X(DIM),
                                               img.shape[2], img.shape[3], 1), 5)
        out["lut_apply_extension_single_thread_1620x1080"] = round(s, 4)
        out["lut_apply_extension_megapixels_per_second"] = round(pixels / s / 1e6, 1)
        out["lut_apply_extension_projected_45mp_single_thread_seconds"] = round(45e6 / (pixels / s), 2)
    s = median_seconds(lambda: tt.forward(lut, img, lc.BINSIZE_MODELS_X(DIM)), 3)
    out["lut_apply_pure_torch_1620x1080"] = round(s, 4)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=lc.RESULTS_DIR / "model_summary.json")
    args = ap.parse_args()
    checkout = lc.checkout_path()
    upstream = lc.verify_checkout(checkout)
    model = um.load(checkout)
    img = um.load_image(checkout / "demo_images" / "sRGB" / "a1629.jpg")
    _, lut = um.fuse(model, img)
    ext = lc.load_extension()

    rows = layer_table(model.classifier, img)
    cnn_params = sum(p.numel() for p in model.classifier.parameters())
    norm_params = sum(r["params"] for r in rows if r["layer"].startswith("InstanceNorm2d"))
    lut_params = sum(t.numel() for t in model.luts)
    conv_macs = sum(r["macs"] for r in rows)
    other = sum(r["other_flops_approx"] for r in rows)
    with FlopCounterMode(display=False) as fc, torch.no_grad():
        model.classifier(img)
    counted = int(fc.get_total_flops())

    per_pixel_ops = {
        "divisions": 6, "floors": 3, "fmods": 3, "subtractions_1_minus_d": 3,
        "weight_multiplies": 16, "lut_multiplies": 24, "lut_additions": 21, "lut_loads": 24,
    }
    report = {
        "schema": "lucent.lut-research.model-summary",
        "schema_version": 1,
        "run_date": dt.date.today().isoformat(),
        "upstream": upstream,
        "environment": lc.environment(),
        "classifier": {
            "class": "models_x.Classifier (identical to models.Classifier)",
            "input": "1 x 3 x H x W float32 RGB in [0, 1] (to_tensor of an 8-bit sRGB image); any H, W",
            "internal_resize": "bilinear to 256 x 256, align_corners=False, no antialiasing; aspect ratio not preserved",
            "output": "3 unconstrained real weights (no softmax, not normalised); demo weights a1629 = "
                      "[1.908, -0.346, -0.928]",
            "layers": rows,
            "params": cnn_params,
            "params_without_instance_norm_affine": cnn_params - norm_params,
            "paper_statement": "'The whole CNN model contains only 269K parameters when N=3' (arXiv 2009.14468); "
                               "269,603 without the 480 InstanceNorm scale/shift parameters, 270,083 with them",
            "conv_macs": conv_macs,
            "conv_flops": 2 * conv_macs,
            "torch_flop_counter_total": counted,
            "torch_flop_counter_matches_conv_flops": counted == 2 * conv_macs,
            "other_flops_approx": other,
            "total_flops_approx": 2 * conv_macs + other,
        },
        "lut_basis": {
            "count": len(model.luts),
            "size": DIM,
            "layout": "float32 [c][b][g][r] (r fastest): output channel c, then blue, green, red input index",
            "params": lut_params,
            "float32_bytes": lut_params * 4,
            "fusion": "LUT = w0*LUT0 + w1*LUT1 + w2*LUT2 (3 x 107,811 multiply-adds per image)",
            "basis_ranges": [[float(t.min()), float(t.max())] for t in model.luts],
        },
        "total_params": cnn_params + lut_params,
        "total_float32_bytes": (cnn_params + lut_params) * 4,
        "trilinear_application": {
            "per_pixel_operations_reference_arithmetic": per_pixel_ops,
            "per_pixel_flops_excluding_loads": sum(v for k, v in per_pixel_ops.items() if k != "lut_loads"),
            "lut_working_set_bytes": 3 * DIM ** 3 * 4,
            "note": "per-pixel and independent; cost scales with output pixels, the classifier's does not",
        },
        "timings": timings(model, img, lut, ext),
        "manifest_fields_draft": {
            "note": "Blueprint section 5 model-manifest fields as they apply to the upstream sRGB model. Research "
                    "record only: not a Lucent asset manifest (ADR 0018) and the weights are not redistributable.",
            "input_domain": "display-referred sRGB (IEC 61966-2-1 encoding), RGB channel order, planar NCHW, "
                            "float32 [0, 1] from 8-bit code values / 255; rendered images only, never camera RGB",
            "output_domain": "display-referred sRGB-encoded RGB, float32, not clamped (about 2-3 % of the demo "
                             "samples fall below 0; the demo clamps when quantising to 8 bits)",
            "training_domain": "MIT-Adobe FiveK, expert C targets, 4,500 training pairs, short side resized to "
                               "480 px, Lightroom-rendered sRGB JPEG inputs; augmentation: random crop 60-100 % "
                               "per side, horizontal flip, brightness and saturation x[0.8, 1.2]",
            "resize_crop": "whole image to the classifier (no crop); internal bilinear 256 x 256 without "
                           "antialiasing, so the weights depend on the resolution of the image supplied",
            "numerical_range": "LUT input domain [0, 1.0000008] for binsize 1.000001/32; outside it the "
                               "upstream code reads out of bounds, so inputs must be clamped to [0, 1]",
            "preprocessing": "PIL decode, no ICC conversion or EXIF orientation, to_tensor (uint8 / 255 in float32)",
            "weights_sha256": {name: lc.sha256_file(checkout / "pretrained_models" / "sRGB" / name)
                               for name in ("LUTs.pth", "classifier.pth")},
            "runtime_opset_provider": "not exported (Appendix D2 step 6 is a later phase)",
            "allowed_placements": "explicit sRGB appearance route only (blueprint section 5)",
        },
    }
    lc.assert_checkout_unchanged(checkout)
    lc.write_json(args.out, report)
    print(f"wrote {args.out}: CNN {cnn_params} params, LUTs {lut_params}, conv MACs {conv_macs}, "
          f"flop counter {'matches' if counted == 2 * conv_macs else 'differs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
