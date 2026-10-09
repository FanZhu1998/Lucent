"""Independent NumPy forward pass of Image-Adaptive-3DLUT's classifier. Research only.

A from-scratch float64 implementation of ``models_x.Classifier`` (identical to ``models.Classifier``)
at b491f6d, written from the layer definitions rather than by calling PyTorch, as the
specification for a native port. ``reproduce_demo.py`` compares it with PyTorch.

Layer sequence (``nn.Sequential`` indices, which name the checkpoint keys ``model.<i>.*``):

  0  Upsample(size=(256, 256), mode='bilinear')   align_corners=False (the default since PyTorch
                                                  0.4.0, so also under the 0.4.1 the repo pins);
                                                  no antialiasing; aspect ratio not preserved
  1  Conv2d(3, 16, 3, stride=2, padding=1)    2  LeakyReLU(0.2)   3  InstanceNorm2d(16, affine)
  4  Conv2d(16, 32, 3, 2, 1)                  5  LeakyReLU(0.2)   6  InstanceNorm2d(32, affine)
  7  Conv2d(32, 64, 3, 2, 1)                  8  LeakyReLU(0.2)   9  InstanceNorm2d(64, affine)
  10 Conv2d(64, 128, 3, 2, 1)                 11 LeakyReLU(0.2)   12 InstanceNorm2d(128, affine)
  13 Conv2d(128, 128, 3, 2, 1)                14 LeakyReLU(0.2)
  15 Dropout(0.5)                             identity in eval()
  16 Conv2d(128, 3, 8, padding=0)             8x8 -> 1x1: three dot products of length 8192

InstanceNorm2d keeps its defaults: eps = 1e-5, track_running_stats=False, so evaluation uses each
image's own per-channel mean and biased variance. Spatial sizes: 256 -> 128 -> 64 -> 32 -> 16 -> 8 -> 1.
The three outputs are the basis-LUT weights, used as they are (no softmax, no normalisation):
LUT = w0 * LUT0 + w1 * LUT1 + w2 * LUT2.
"""

from __future__ import annotations

from typing import Any

import numpy as np

CONV_LAYERS = {1: (2, 1), 4: (2, 1), 7: (2, 1), 10: (2, 1), 13: (2, 1), 16: (1, 0)}  # index: (stride, pad)
NORM_LAYERS = (3, 6, 9, 12)
LEAKY_SLOPE = 0.2
IN_EPS = 1e-5
MODEL_SIZE = 256


def resize_bilinear(x: np.ndarray, out_h: int, out_w: int) -> np.ndarray:
    """PyTorch upsample_bilinear2d with align_corners=False and no antialiasing, in float64:
    src = (dst + 0.5) * in/out - 0.5, clamped below at 0; the right/bottom neighbour is clamped
    to the last row/column."""

    def taps(n_in: int, n_out: int):
        src = (np.arange(n_out, dtype=np.float64) + 0.5) * (n_in / n_out) - 0.5
        src = np.maximum(src, 0.0)
        i0 = np.minimum(np.floor(src).astype(np.int64), n_in - 1)
        i1 = np.minimum(i0 + 1, n_in - 1)
        l1 = src - i0
        return i0, i1, 1.0 - l1, l1

    x = np.asarray(x, dtype=np.float64)
    _, h, w = x.shape
    i0h, i1h, l0h, l1h = taps(h, out_h)
    i0w, i1w, l0w, l1w = taps(w, out_w)
    rows = x[:, i0h, :] * l0h[None, :, None] + x[:, i1h, :] * l1h[None, :, None]
    return rows[:, :, i0w] * l0w[None, None, :] + rows[:, :, i1w] * l1w[None, None, :]


def conv2d(x: np.ndarray, weight: np.ndarray, bias: np.ndarray, stride: int, pad: int) -> np.ndarray:
    cin, h, w = x.shape
    cout, cin_w, kh, kw = weight.shape
    assert cin == cin_w
    xp = np.pad(x, ((0, 0), (pad, pad), (pad, pad)))
    ho = (h + 2 * pad - kh) // stride + 1
    wo = (w + 2 * pad - kw) // stride + 1
    out = np.zeros((cout, ho, wo), dtype=np.float64)
    for i in range(kh):
        for j in range(kw):
            patch = xp[:, i : i + stride * (ho - 1) + 1 : stride, j : j + stride * (wo - 1) + 1 : stride]
            out += np.tensordot(weight[:, :, i, j], patch, axes=([1], [0]))
    return out + bias[:, None, None]


def leaky_relu(x: np.ndarray) -> np.ndarray:
    return np.where(x > 0, x, x * LEAKY_SLOPE)


def instance_norm(x: np.ndarray, gamma: np.ndarray, beta: np.ndarray) -> np.ndarray:
    mean = x.mean(axis=(1, 2), keepdims=True)
    var = ((x - mean) ** 2).mean(axis=(1, 2), keepdims=True)  # biased, as PyTorch normalises
    return (x - mean) / np.sqrt(var + IN_EPS) * gamma[:, None, None] + beta[:, None, None]


def forward(state: dict[str, Any], image: np.ndarray) -> tuple[np.ndarray, list[dict[str, Any]]]:
    """state: checkpoint tensors (``model.<i>.weight``/``bias``) as arrays; image: (3, H, W) in
    [0, 1] (to_tensor output). Returns the three weights (float64) and a per-layer trace."""
    p = {k: np.asarray(v.detach().cpu().numpy() if hasattr(v, "detach") else v, dtype=np.float64)
         for k, v in state.items()}
    trace: list[dict[str, Any]] = []
    x = resize_bilinear(image, MODEL_SIZE, MODEL_SIZE)
    trace.append({"layer": 0, "op": "upsample_bilinear", "out": list(x.shape)})
    for i in range(1, 17):
        if i in CONV_LAYERS:
            stride, pad = CONV_LAYERS[i]
            x = conv2d(x, p[f"model.{i}.weight"], p[f"model.{i}.bias"], stride, pad)
            trace.append({"layer": i, "op": "conv2d", "out": list(x.shape)})
        elif i in NORM_LAYERS:
            x = instance_norm(x, p[f"model.{i}.weight"], p[f"model.{i}.bias"])
            trace.append({"layer": i, "op": "instance_norm", "out": list(x.shape)})
        elif i == 15:
            trace.append({"layer": i, "op": "dropout(eval: identity)", "out": list(x.shape)})
        else:
            x = leaky_relu(x)
            trace.append({"layer": i, "op": "leaky_relu", "out": list(x.shape)})
    return x.reshape(-1), trace


def fuse_luts(weights: np.ndarray, luts: list[np.ndarray]) -> np.ndarray:
    """demo_eval.generate_LUT in float32: pred[0]*LUT0 + pred[1]*LUT1 + pred[2]*LUT2, left to right."""
    w = np.asarray(weights, dtype=np.float32)
    out = w[0] * luts[0]
    for k in range(1, len(luts)):
        out = out + w[k] * luts[k]
    return out.astype(np.float32)
