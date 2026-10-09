"""Pure-PyTorch trilinear LUT sampler with the CPU extension's exact arithmetic. Research only.

``forward`` reproduces ``TriLinearForwardCpu`` (trilinear_cpp/src/trilinear.cpp at b491f6d) with
vectorised float32 tensor operations in the same order: cell index ``floor(x / binsize)``,
fraction ``fmod(x, binsize) / binsize``, the eight weights as left-to-right products and one
left-to-right sum of eight terms per output channel, with the LUT laid out as lut[c][b][g][r].
Every step is a separate correctly rounded IEEE float32 operation, so results are bit-identical
to the C++ loop compiled without contraction or reassociation (MSVC /fp:precise); see
trilinear_parity.py for the evidence.

``ExtensionApi`` exposes it under the extension's module API (``forward(lut, image, output,
lut_dim, shift, binsize, width, height, batch)`` returning 1, or 0 when the image does not have
3 channels) so models_x.py can import it as ``trilinear`` without the compiled module. Unlike the
extension it refuses batches larger than one (the extension leaves those outputs unwritten) and
inputs outside the LUT (the extension reads out of bounds). It is inference-only.

This is not grid_sample: torch.nn.functional.grid_sample (suggested in upstream issue 14) uses
a different coordinate mapping and does not reproduce the binsize and fmod arithmetic exactly.
"""

from __future__ import annotations

import types

import torch


class DomainError(ValueError):
    """Input the extension would turn into an out-of-bounds LUT read."""


def forward(lut: torch.Tensor, image: torch.Tensor, binsize: float) -> torch.Tensor:
    """lut: float32 (3, D, D, D) laid out [c][b][g][r]; image: float32 (3, H, W) or (1, 3, H, W)."""
    if lut.dtype != torch.float32 or image.dtype != torch.float32:
        raise TypeError("lut and image must be float32")
    if lut.dim() != 4 or lut.shape[0] != 3 or not (lut.shape[1] == lut.shape[2] == lut.shape[3]):
        raise ValueError(f"lut must have shape (3, D, D, D), got {tuple(lut.shape)}")
    squeeze = image.dim() == 4
    if squeeze:
        if image.shape[0] != 1:
            raise ValueError("one image at a time (the extension's CPU path ignores the batch)")
        image = image[0]
    if image.dim() != 3 or image.shape[0] != 3:
        raise ValueError(f"image must have 3 channels, got {tuple(image.shape)}")

    dim = lut.shape[-1]
    shift = dim * dim * dim
    flat = lut.detach().contiguous().reshape(-1)
    x = image.detach().contiguous().reshape(3, -1)
    bs = torch.tensor(binsize, dtype=torch.float32)  # rounds to nearest, like pybind11's float cast

    if not torch.isfinite(x).all():
        raise DomainError("non-finite input")
    ids = torch.floor(x / bs).to(torch.int64)
    if ids.numel() and (int(ids.min()) < 0 or int(ids.max()) > dim - 2):
        raise DomainError(
            f"input outside the LUT domain: cell index range [{int(ids.min())}, {int(ids.max())}], "
            f"allowed [0, {dim - 2}]"
        )
    fr = torch.fmod(x, bs) / bs
    r_id, g_id, b_id = ids[0], ids[1], ids[2]
    r_d, g_d, b_d = fr[0], fr[1], fr[2]

    id000 = r_id + g_id * dim + b_id * dim * dim
    id100 = r_id + 1 + g_id * dim + b_id * dim * dim
    id010 = r_id + (g_id + 1) * dim + b_id * dim * dim
    id110 = r_id + 1 + (g_id + 1) * dim + b_id * dim * dim
    id001 = r_id + g_id * dim + (b_id + 1) * dim * dim
    id101 = r_id + 1 + g_id * dim + (b_id + 1) * dim * dim
    id011 = r_id + (g_id + 1) * dim + (b_id + 1) * dim * dim
    id111 = r_id + 1 + (g_id + 1) * dim + (b_id + 1) * dim * dim

    w000 = (1 - r_d) * (1 - g_d) * (1 - b_d)
    w100 = r_d * (1 - g_d) * (1 - b_d)
    w010 = (1 - r_d) * g_d * (1 - b_d)
    w110 = r_d * g_d * (1 - b_d)
    w001 = (1 - r_d) * (1 - g_d) * b_d
    w101 = r_d * (1 - g_d) * b_d
    w011 = (1 - r_d) * g_d * b_d
    w111 = r_d * g_d * b_d

    out = torch.empty_like(x)
    for c in range(3):
        o = c * shift
        out[c] = (
            w000 * flat[id000 + o] + w100 * flat[id100 + o]
            + w010 * flat[id010 + o] + w110 * flat[id110 + o]
            + w001 * flat[id001 + o] + w101 * flat[id101 + o]
            + w011 * flat[id011 + o] + w111 * flat[id111 + o]
        )
    out = out.reshape(image.shape)
    return out.unsqueeze(0) if squeeze else out


class ExtensionApi(types.ModuleType):
    """Module object with the compiled extension's API, backed by ``forward`` above."""

    def __init__(self, name: str = "trilinear") -> None:
        super().__init__(name)

    @staticmethod
    def forward(lut, image, output, lut_dim, shift, binsize, width, height, batch) -> int:  # noqa: D102
        if image.dim() != 4 or image.shape[1] != 3:
            return 0  # the extension's own refusal: "whether color image"
        if batch != 1 or image.shape[0] != 1:
            raise NotImplementedError("batch > 1: the extension's CPU path writes only the first image")
        if lut.shape[-1] != lut_dim or shift != lut_dim ** 3 or width * height != image.shape[2] * image.shape[3]:
            raise ValueError("inconsistent lut_dim/shift/width/height arguments")
        if not (lut.is_contiguous() and image.is_contiguous() and output.is_contiguous()):
            raise ValueError("the extension reads raw contiguous buffers")
        with torch.no_grad():
            output.copy_(forward(lut, image, binsize))
        return 1

    @staticmethod
    def backward(*args, **kwargs) -> int:  # noqa: D102
        raise NotImplementedError("the research sampler is inference-only")
