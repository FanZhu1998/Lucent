"""Independent NumPy reference of Image-Adaptive-3DLUT's CPU trilinear forward pass. Research only.

Mirrors ``TriLinearForwardCpu`` in ``trilinear_cpp/src/trilinear.cpp`` (its body is identical to
``trilinear_c/src/trilinear.c``) at commit b491f6d, operation by operation, in float32:

    r_id = floor(r / binsize)                    float32 division, then floor
    r_d  = fmod(r, binsize) / binsize            exact fmod, then float32 division
    id000 = r_id + g_id*dim + b_id*dim*dim       the LUT is lut[channel][b][g][r], r fastest
    w000 = (1-r_d)*(1-g_d)*(1-b_d), ..., w111    products evaluated left to right
    out_c = w000*lut[id000 + c*shift] + w100*lut[id100 + c*shift] + ... + w111*lut[id111 + c*shift]
                                                 one left-to-right float32 sum per channel

``binsize`` reaches the C++ code as a ``float``: pybind11 rounds the Python double to nearest, as
``np.float32(binsize)`` does. The caller (models_x.py) passes ``1.000001 / (dim - 1)``; the legacy
models.py passes ``1.0001 / (dim - 1)``. Upper inputs therefore stay below the last cell boundary.

The C++ code has no bounds checks and reads outside the LUT for inputs outside the cube (or NaN).
This reference raises :class:`DomainError` for them instead of guessing undefined behaviour.
It processes one image (3, H, W); the extension's CPU path ignores ``batch`` and writes only the
first image of a batch.
"""

from __future__ import annotations

import numpy as np

LUT_LAYOUT = "lut[c, b, g, r] float32, C-contiguous; output channel c reads lut[c]; r index fastest"


class DomainError(ValueError):
    """Input the extension would turn into an out-of-bounds LUT read."""


def _check(lut: np.ndarray, image: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if not isinstance(lut, np.ndarray) or lut.dtype != np.float32:
        raise TypeError("lut must be a float32 numpy array")
    if lut.ndim != 4 or lut.shape[0] != 3 or not (lut.shape[1] == lut.shape[2] == lut.shape[3]):
        raise ValueError(f"lut must have shape (3, D, D, D), got {lut.shape}")
    if not isinstance(image, np.ndarray) or image.dtype != np.float32:
        raise TypeError("image must be a float32 numpy array")
    if image.ndim != 3 or image.shape[0] != 3:
        raise ValueError(f"image must have shape (3, H, W), got {image.shape}")
    return np.ascontiguousarray(lut), np.ascontiguousarray(image)


FRACTION_MODES = ("reference", "quotient", "exact_remainder")


def _cell(x: np.ndarray, bs: np.float32, dim: int, mode: str) -> tuple[np.ndarray, np.ndarray]:
    if not np.isfinite(x).all():
        raise DomainError("non-finite input")
    q = x / bs
    idx = np.floor(q).astype(np.int64)
    if idx.size and (idx.min() < 0 or idx.max() > dim - 2):
        raise DomainError(
            f"input outside the LUT domain: cell index range [{idx.min()}, {idx.max()}], allowed "
            f"[0, {dim - 2}] (the C++ code would read outside the LUT)"
        )
    if mode == "reference":
        frac = np.fmod(x, bs) / bs
    elif mode == "quotient":
        frac = q - idx.astype(np.float32)
    elif mode == "exact_remainder":
        # x - idx*bs is exact in double (both terms have at most 30 significant bits), and for
        # float32 operands a double division rounded to float32 equals the float32 division.
        # Where idx is the exact floor this is upstream's fmod fraction bit for bit; where float32
        # rounding made idx one too high the remainder is a tiny negative number and the clamp
        # gives fraction 0, the continuous answer.
        r = x.astype(np.float64) - idx.astype(np.float64) * np.float64(bs)
        frac = np.clip(r / np.float64(bs), 0.0, 1.0).astype(np.float32)
    else:
        raise ValueError(f"unknown fraction mode {mode!r}")
    return idx, frac


def _forward(lut: np.ndarray, image: np.ndarray, binsize: float, chunk: int, mode: str) -> np.ndarray:
    lut, image = _check(lut, image)
    dim = lut.shape[-1]
    shift = dim * dim * dim
    flat = lut.reshape(-1)
    bs = np.float32(binsize)
    one = np.float32(1.0)
    planes = image.reshape(3, -1)
    n = planes.shape[1]
    out = np.empty((3, n), dtype=np.float32)
    for s in range(0, n, chunk):
        e = min(n, s + chunk)
        r_id, r_d = _cell(planes[0, s:e], bs, dim, mode)
        g_id, g_d = _cell(planes[1, s:e], bs, dim, mode)
        b_id, b_d = _cell(planes[2, s:e], bs, dim, mode)

        id000 = r_id + g_id * dim + b_id * dim * dim
        id100 = r_id + 1 + g_id * dim + b_id * dim * dim
        id010 = r_id + (g_id + 1) * dim + b_id * dim * dim
        id110 = r_id + 1 + (g_id + 1) * dim + b_id * dim * dim
        id001 = r_id + g_id * dim + (b_id + 1) * dim * dim
        id101 = r_id + 1 + g_id * dim + (b_id + 1) * dim * dim
        id011 = r_id + (g_id + 1) * dim + (b_id + 1) * dim * dim
        id111 = r_id + 1 + (g_id + 1) * dim + (b_id + 1) * dim * dim

        w000 = (one - r_d) * (one - g_d) * (one - b_d)
        w100 = r_d * (one - g_d) * (one - b_d)
        w010 = (one - r_d) * g_d * (one - b_d)
        w110 = r_d * g_d * (one - b_d)
        w001 = (one - r_d) * (one - g_d) * b_d
        w101 = r_d * (one - g_d) * b_d
        w011 = (one - r_d) * g_d * b_d
        w111 = r_d * g_d * b_d

        for c in range(3):
            o = c * shift
            out[c, s:e] = (
                w000 * flat[id000 + o] + w100 * flat[id100 + o]
                + w010 * flat[id010 + o] + w110 * flat[id110 + o]
                + w001 * flat[id001 + o] + w101 * flat[id101 + o]
                + w011 * flat[id011 + o] + w111 * flat[id111 + o]
            )
    return out.reshape(image.shape)


def forward(lut: np.ndarray, image: np.ndarray, binsize: float, chunk: int = 1 << 20) -> np.ndarray:
    """The extension's arithmetic exactly (bit-for-bit target)."""
    return _forward(lut, image, binsize, chunk, mode="reference")


def forward_consistent_fraction(
    lut: np.ndarray, image: np.ndarray, binsize: float, chunk: int = 1 << 20
) -> np.ndarray:
    """Analysis variant: fraction = q - floor(q) with q = x / binsize, so index and fraction come
    from the same quotient. Continuous, but q carries float32 rounding (up to about 1e-6 near
    q = 31), so it differs from the reference by up to a few 1e-6 on steep LUTs."""
    return _forward(lut, image, binsize, chunk, mode="quotient")


def forward_exact_remainder(
    lut: np.ndarray, image: np.ndarray, binsize: float, chunk: int = 1 << 20
) -> np.ndarray:
    """Candidate production arithmetic: index floor(x / binsize) as the reference, fraction from
    the exact remainder (x - index * binsize) / binsize computed in double and clamped to [0, 1].
    Bit-identical to the reference wherever the reference's index and fraction agree (every 8-bit
    and 16-bit code value, and all but 8 float32 values in [0, 1]); continuous at those 8."""
    return _forward(lut, image, binsize, chunk, mode="exact_remainder")


def forward_ideal_float64(lut: np.ndarray, image: np.ndarray) -> np.ndarray:
    """The mathematically intended trilinear interpolation, in float64: grid nodes at k/(D-1),
    inputs clamped to [0, 1], the top face handled by the last cell with fraction 1. Error
    analysis only; not a parity target."""
    lut, image = _check(lut, image)
    dim = lut.shape[-1]
    L = lut.astype(np.float64)
    x = np.clip(image.astype(np.float64).reshape(3, -1), 0.0, 1.0) * (dim - 1)
    i = np.minimum(np.floor(x), dim - 2).astype(np.int64)
    f = x - i
    ri, gi, bi = i
    rf, gf, bf = f
    out = np.zeros((3, x.shape[1]), dtype=np.float64)
    for db in (0, 1):
        wb = bf if db else 1.0 - bf
        for dg in (0, 1):
            wg = gf if dg else 1.0 - gf
            for dr in (0, 1):
                wr = rf if dr else 1.0 - rf
                out += (wr * wg * wb) * L[:, bi + db, gi + dg, ri + dr]
    return out.reshape(image.shape)


def index_census(x: np.ndarray, binsize: float) -> dict[str, np.ndarray]:
    """For float32 inputs, compares the reference's cell index floor(fl32(x / binsize)) with the
    exact floor of the real quotient. float64 division gives the exact floor here: for float32
    x and binsize a non-integer quotient is at least 2**-24 away from the next integer, far more
    than float64 rounding near 33."""
    x = np.asarray(x, dtype=np.float32)
    bs = np.float32(binsize)
    k_ref = np.floor(x / bs).astype(np.int64)
    k_exact = np.floor(x.astype(np.float64) / np.float64(bs)).astype(np.int64)
    frac_ref = np.fmod(x, bs) / bs
    return {"k_ref": k_ref, "k_exact": k_exact, "frac_ref": frac_ref, "inconsistent": k_ref != k_exact}
