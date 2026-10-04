"""Independent check of Lucent's DNG opcode processing against rawpy/LibRaw (ADR 0014).

LibRaw does not apply DNG opcode lists (its DNG SDK integration is not built), so its output
cannot be compared with Lucent's directly. This script therefore builds the reference itself:

  1. rawpy 0.27.1 / LibRaw renders the DNG in the camera's own colour space (output_color=raw),
     linear, as-shot white balance, highlight clip, no brightening, no rotation: the analogue of
     Lucent's stage one before the opcodes (LibRaw demosaics with AHD, Lucent with
     Malvar-He-Cutler).
  2. The file's OpcodeList3 is read here with a separate minimal TIFF parser and applied to that
     image with NumPy/SciPy, written from the DNG 1.7.1.0 chapter 7 formulas (FixVignetteRadial,
     GainMap, WarpRectilinear resampled with SciPy's cubic B-spline, a different kernel from
     Lucent's Catmull-Rom). OpcodeList1 (bad pixels) is not reproduced: it changes a few
     thousand isolated pixels and is invisible at the comparison scale.
  3. Lucent's stage one with and without the opcodes (dumped by
     lucent_engine_fixture_tests --gtest_filter=DngOpcodeFixtures.* with
     LUCENT_OPCODE_DUMP_DIR set) is compared with the reference, after and before the opcodes:
       photometric: 32 x 32-pixel block means per plane, ratio Lucent / reference normalised by
         its median (the two decoders scale differently by a constant), reported as percentiles
         of |ratio - 1| over blocks that are not clipped or dark;
       geometric: sub-pixel shifts between 64 x 64 patches (phase correlation, green plane) on a
         grid; Lucent with opcodes against the reference with opcodes should be ~0, against the
         reference without opcodes it shows the warp.
  4. Reduced JPEG previews (before / after, and corner crops at full resolution side by side) for
     visual inspection.

Usage (research/.venv):
    python tests/tools/dng_opcode_reference.py DUMP_DIR [--out OUT_DIR]
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
import rawpy
from PIL import Image
from scipy import ndimage

TYPE_SIZES = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8, 13: 4, 16: 8, 17: 8, 18: 8}


# ---- Minimal TIFF / opcode parser (independent of Lucent's) -----------------------------------------

def raw_ifd_opcode_lists(path: Path) -> dict[int, bytes]:
    """Opcode-list payloads of the IFD holding NewSubFileType 0 and a CFA/LinearRaw image."""
    data = path.read_bytes()
    bo = "<" if data[:2] == b"II" else ">"
    seen: set[int] = set()
    found: dict[int, bytes] = {}

    def walk(offset: int) -> int:
        if offset == 0 or offset in seen or offset >= len(data):
            return 0
        seen.add(offset)
        (count,) = struct.unpack(bo + "H", data[offset:offset + 2])
        tags = {}
        for i in range(count):
            e = offset + 2 + 12 * i
            tag, typ, cnt = struct.unpack(bo + "HHI", data[e:e + 8])
            size = TYPE_SIZES.get(typ, 1) * cnt
            voff = e + 8 if size <= 4 else struct.unpack(bo + "I", data[e + 8:e + 12])[0]
            tags[tag] = (typ, cnt, voff, size)
        subfile = struct.unpack(bo + "I", data[tags[254][2]:tags[254][2] + 4])[0] if 254 in tags else 0
        photometric = struct.unpack(bo + "H", data[tags[262][2]:tags[262][2] + 2])[0] if 262 in tags else 0
        if subfile == 0 and photometric in (32803, 34892):
            for tag, number in ((51008, 1), (51009, 2), (51022, 3)):
                if tag in tags:
                    _, _, voff, size = tags[tag]
                    found[number] = data[voff:voff + size]
        if 330 in tags:
            _, cnt, voff, _ = tags[330]
            for k in range(cnt):
                walk(struct.unpack(bo + "I", data[voff + 4 * k:voff + 4 * k + 4])[0])
        return struct.unpack(bo + "I", data[offset + 2 + 12 * count:offset + 6 + 12 * count])[0]

    nxt = struct.unpack(bo + "I", data[4:8])[0]
    while nxt:
        nxt = walk(nxt)
    return found


def opcodes(payload: bytes) -> list[tuple[int, bytes]]:
    (n,) = struct.unpack(">I", payload[:4])
    pos = 4
    out = []
    for _ in range(n):
        oid, _version, _flags, size = struct.unpack(">IIII", payload[pos:pos + 16])
        pos += 16
        out.append((oid, payload[pos:pos + size]))
        pos += size
    return out


# ---- Opcodes from the specification ------------------------------------------------------------------

def centre_and_m(w: int, h: int, chx: float, chy: float) -> tuple[float, float, float]:
    x0, y0, x1, y1 = 0.0, 0.0, w - 1.0, h - 1.0  # top-left and bottom-right pixel coordinates
    cx = x0 + chx * (x1 - x0)
    cy = y0 + chy * (y1 - y0)
    mx = max(abs(x0 - cx), abs(x1 - cx))
    my = max(abs(y0 - cy), abs(y1 - cy))
    return cx, cy, float(np.sqrt(mx * mx + my * my))


def fix_vignette_radial(img: np.ndarray, p: bytes) -> np.ndarray:
    k0, k1, k2, k3, k4, chx, chy = struct.unpack(">7d", p)
    h, w, _ = img.shape
    cx, cy, m = centre_and_m(w, h, chx, chy)
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    r = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / m
    g = 1 + k0 * r**2 + k1 * r**4 + k2 * r**6 + k3 * r**8 + k4 * r**10
    return img * g[..., None]


def gain_map(img: np.ndarray, p: bytes) -> np.ndarray:
    top, left, bottom, right, plane, planes, rowpitch, colpitch, pv, ph = struct.unpack(">10I", p[:40])
    sv, sh, ov, oh = struct.unpack(">4d", p[40:72])
    (mp,) = struct.unpack(">I", p[72:76])
    gains = np.frombuffer(p[76:], dtype=">f4").astype(np.float64).reshape(pv, ph, mp)
    h, w, nplanes = img.shape
    out = img.copy()
    rows = np.arange(top, min(bottom, h), rowpitch)
    cols = np.arange(left, min(right, w), colpitch)
    # Map coordinates of pixel centres, relative to the image, clamped to the grid.
    v = np.clip(((rows + 0.5) / h - ov) / sv if pv > 1 else np.zeros(len(rows)), 0, pv - 1)
    u = np.clip(((cols + 0.5) / w - oh) / sh if ph > 1 else np.zeros(len(cols)), 0, ph - 1)
    for i in range(planes):
        p_img = plane + i
        if p_img >= nplanes:
            break
        g = gains[:, :, min(i, mp - 1)]
        vv, uu = np.meshgrid(v, u, indexing="ij")
        gm = ndimage.map_coordinates(g, [vv, uu], order=1, mode="nearest")
        out[np.ix_(rows, cols, [p_img])] *= gm[..., None]
    return out


def warp_rectilinear(img: np.ndarray, p: bytes) -> np.ndarray:
    (n,) = struct.unpack(">I", p[:4])
    sets = [struct.unpack(">6d", p[4 + 48 * i:4 + 48 * (i + 1)]) for i in range(n)]
    chx, chy = struct.unpack(">2d", p[4 + 48 * n:4 + 48 * n + 16])
    h, w, nplanes = img.shape
    cx, cy, m = centre_and_m(w, h, chx, chy)
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    dx = (x - cx) / m
    dy = (y - cy) / m
    r2 = dx * dx + dy * dy
    out = np.empty_like(img)
    for i in range(nplanes):
        kr0, kr1, kr2, kr3, kt0, kt1 = sets[min(i, n - 1)]
        f = kr0 + kr1 * r2 + kr2 * r2**2 + kr3 * r2**3
        xs = cx + m * (f * dx + kt0 * 2 * dx * dy + kt1 * (r2 + 2 * dx * dx))
        ys = cy + m * (f * dy + kt1 * 2 * dx * dy + kt0 * (r2 + 2 * dy * dy))
        out[..., i] = ndimage.map_coordinates(img[..., i], [ys, xs], order=3, mode="nearest")
    return out


APPLY = {3: fix_vignette_radial, 9: gain_map, 1: warp_rectilinear}
NAMES = {1: "WarpRectilinear", 3: "FixVignetteRadial", 5: "FixBadPixelsList", 9: "GainMap"}


# ---- Comparison ------------------------------------------------------------------------------------------

def block_means(img: np.ndarray, b: int) -> np.ndarray:
    h, w, c = img.shape
    h2, w2 = h // b * b, w // b * b
    return img[:h2, :w2].reshape(h2 // b, b, w2 // b, b, c).mean(axis=(1, 3))


def photometric(lucent: np.ndarray, reference: np.ndarray, b: int = 32) -> dict:
    a = block_means(lucent, b)
    r = block_means(reference, b)
    ok = (r > 0.02) & (r < 0.9) & (a > 0.02) & (a < 0.9)  # not dark, not clipped
    ratio = np.where(ok, a / np.where(ok, r, 1), np.nan)
    stats = {}
    for c, name in enumerate("RGB"):
        rc = ratio[..., c]
        rc = rc[np.isfinite(rc)]
        med = float(np.median(rc))
        dev = np.abs(rc / med - 1)
        stats[name] = {"blocks": int(rc.size), "median_ratio": med,
                       "p50": float(np.percentile(dev, 50)), "p95": float(np.percentile(dev, 95)),
                       "p99": float(np.percentile(dev, 99)), "max": float(dev.max())}
    return stats


def phase_shift(a: np.ndarray, b: np.ndarray) -> tuple[float, float, float]:
    """Sub-pixel (dy, dx) such that b(p + d) ~ a(p); and the peak strength."""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa = np.fft.fft2((a - a.mean()) * win)
    fb = np.fft.fft2((b - b.mean()) * win)
    cps = fb * np.conj(fa)
    cps /= np.abs(cps) + 1e-12
    corr = np.real(np.fft.ifft2(cps))
    iy, ix = np.unravel_index(np.argmax(corr), corr.shape)
    peak = corr[iy, ix]

    def sub(c_m, c_0, c_p):
        d = c_m - 2 * c_0 + c_p
        return 0.5 * (c_m - c_p) / d if d < 0 else 0.0

    n0, n1 = corr.shape
    dy = iy + sub(corr[(iy - 1) % n0, ix], peak, corr[(iy + 1) % n0, ix])
    dx = ix + sub(corr[iy, (ix - 1) % n1], peak, corr[iy, (ix + 1) % n1])
    dy = dy - n0 if dy > n0 / 2 else dy
    dx = dx - n1 if dx > n1 / 2 else dx
    return float(dy), float(dx), float(peak)


def geometric(lucent: np.ndarray, reference: np.ndarray, size: int = 64) -> dict:
    h, w, _ = lucent.shape
    shifts = []
    for gy in np.linspace(0.08, 0.92, 5):
        for gx in np.linspace(0.08, 0.92, 7):
            y0, x0 = int(gy * h) - size // 2, int(gx * w) - size // 2
            a = lucent[y0:y0 + size, x0:x0 + size, 1].astype(np.float64)
            r = reference[y0:y0 + size, x0:x0 + size, 1].astype(np.float64)
            if a.std() < 3e-3:
                continue
            dy, dx, peak = phase_shift(a, r)
            if peak < 0.1:
                continue
            shifts.append({"x": x0 + size / 2, "y": y0 + size / 2, "dx": dx, "dy": dy, "peak": peak})
    mags = [np.hypot(s["dx"], s["dy"]) for s in shifts]
    return {"patches": len(shifts), "median_shift": float(np.median(mags)) if mags else None,
            "max_shift": float(np.max(mags)) if mags else None, "shifts": shifts}


def display(img: np.ndarray, white: float) -> np.ndarray:
    v = np.clip(img / white, 0, 1)
    v = np.where(v <= 0.0031308, 12.92 * v, 1.055 * np.power(v, 1 / 2.4) - 0.055)
    return (v * 255 + 0.5).astype(np.uint8)


def previews(fid: str, lucent_with: np.ndarray, lucent_plain: np.ndarray, out: Path) -> None:
    white = float(np.percentile(lucent_plain[..., 1], 99.5)) * 1.1
    for name, img in (("plain", lucent_plain), ("with", lucent_with)):
        small = block_means(img, 4)
        Image.fromarray(display(small, white)).save(out / f"{fid}-{name}-quarter.jpg", quality=90)
    # Corner crops at full resolution: top-left 600 x 400 before | after, and the difference.
    h, w, _ = lucent_with.shape
    cw, ch = min(600, w // 4), min(400, h // 4)
    crops = []
    for (y0, x0) in ((0, 0), (h - ch, w - cw)):
        before = display(lucent_plain[y0:y0 + ch, x0:x0 + cw], white)
        after = display(lucent_with[y0:y0 + ch, x0:x0 + cw], white)
        gap = np.full((ch, 8, 3), 255, np.uint8)
        crops.append(np.concatenate([before, gap, after], axis=1))
    sheet = np.concatenate([crops[0], np.full((8, crops[0].shape[1], 3), 255, np.uint8), crops[1]], axis=0)
    Image.fromarray(sheet).save(out / f"{fid}-corners-before-after.jpg", quality=92)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or args.dump
    out.mkdir(parents=True, exist_ok=True)
    results = {"rawpy": rawpy.__version__, "libraw": rawpy.libraw_version, "fixtures": {}}
    for sidecar in sorted(args.dump.glob("*.json")):
        if sidecar.name == "dng_opcode_reference.json":
            continue
        fid = sidecar.stem
        meta = json.loads(sidecar.read_text())
        dng = Path(meta["file"])
        print(f"== {fid}", flush=True)
        lucent_with = np.load(args.dump / f"{fid}-with.npy")
        lucent_plain = np.load(args.dump / f"{fid}-plain.npy")
        with rawpy.imread(str(dng)) as raw:
            ref = raw.postprocess(output_color=rawpy.ColorSpace.raw, gamma=(1, 1), no_auto_bright=True,
                                  use_camera_wb=True, output_bps=16, user_flip=0,
                                  highlight_mode=rawpy.HighlightMode.Clip, adjust_maximum_thr=0.0)
        ref = ref.astype(np.float64) / 65535.0
        if ref.shape != lucent_with.shape:
            print(f"   size mismatch: rawpy {ref.shape}, Lucent {lucent_with.shape}; cropping to the common area")
            hh, ww = min(ref.shape[0], lucent_with.shape[0]), min(ref.shape[1], lucent_with.shape[1])
            ref = ref[:hh, :ww]
            lucent_with = lucent_with[:hh, :ww]
            lucent_plain = lucent_plain[:hh, :ww]
        lists = raw_ifd_opcode_lists(dng)
        applied = []
        ref_after = ref.copy()
        for oid, params in opcodes(lists.get(3, b"\x00\x00\x00\x00")):
            if oid in APPLY:
                ref_after = APPLY[oid](ref_after, params)
                applied.append(NAMES[oid])
        print(f"   reference applies OpcodeList3: {applied}")
        record = {
            "opcodes_list3": applied,
            "opcodes_list1": [NAMES.get(o, str(o)) for o, _ in opcodes(lists[1])] if 1 in lists else [],
            "photometric": {
                "with_vs_reference_with": photometric(lucent_with, ref_after),
                "with_vs_reference_without": photometric(lucent_with, ref),
                "plain_vs_reference_without": photometric(lucent_plain, ref),
            },
            "geometric": {
                "with_vs_reference_with": geometric(lucent_with, ref_after),
                "with_vs_reference_without": geometric(lucent_with, ref),
                "plain_vs_reference_without": geometric(lucent_plain, ref),
            },
        }
        for key, value in record["photometric"].items():
            g = value["G"]
            print(f"   photometric {key:28s} G: median ratio {g['median_ratio']:.4f}, |dev| p50 {g['p50']:.4f} "
                  f"p95 {g['p95']:.4f} p99 {g['p99']:.4f} max {g['max']:.4f} ({g['blocks']} blocks)")
        for key, value in record["geometric"].items():
            print(f"   geometric   {key:28s} {value['patches']} patches: median shift {value['median_shift']} px, "
                  f"max {value['max_shift']} px")
        previews(fid, lucent_with, lucent_plain, out)
        results["fixtures"][fid] = record
    (out / "dng_opcode_reference.json").write_text(json.dumps(results, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
