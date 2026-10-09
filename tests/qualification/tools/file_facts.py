#!/usr/bin/env python3
"""Independent file facts for camera qualification expectations (ADR 0016).

Reads, with the Python standard library only and without LibRaw or any Lucent code, the facts a
RAW file declares about itself that fixtures/qualification/expectations.json cites as
"file_tag" evidence:

  * TIFF-family files (DNG, NEF, ARW, CR2, PEF, 3FR, ORF "IIRO", RW2 "IIU"): every IFD of the IFD 0
    chain, SubIFDs and the Exif IFD; Make, Model, Orientation, NewSubFileType, dimensions,
    BitsPerSample, Compression, PhotometricInterpretation, SamplesPerPixel, TIFF/EP and EXIF
    CFAPattern (33421/33422, 41730), DNG CFAPlaneColor/CFALayout, ActiveArea, DefaultCropOrigin
    and DefaultCropSize, DefaultUserCrop, and Panasonic RW2 sensor borders and CFA code.
  * The camera's own crop declaration in two maker notes (W12, rotated fixtures; names as in
    ExifTool's tables): Canon SensorInfo (Canon maker note tag 0x00e0, int16u: [1] SensorWidth,
    [2] SensorHeight, [5] SensorLeftBorder, [6] SensorTopBorder, [7] SensorRightBorder,
    [8] SensorBottomBorder, borders inclusive; https://exiftool.org/TagNames/Canon.html#SensorInfo)
    and Olympus ImageProcessing (maker note tag 0x2040, a sub-IFD) tags 0x0612 CropLeft, 0x0613
    CropTop, 0x0614 CropWidth and 0x0615 CropHeight
    (https://exiftool.org/TagNames/Olympus.html#ImageProcessing).
  * CR3: the CMT1 TIFF stream (Make, Model, Orientation) and every CMP1 box (CRX image width,
    height, bits per sample, plane count, CFA layout and wavelet levels, as documented by
    https://github.com/lclevy/canon_cr3 "CMP1": CFA layout 0 = RGGB, 1 = GRBG, 2 = GBRG,
    3 = BGGR; "number of wavelet levels ... 0 for raw, 3 for craw").
  * RAF: the camera string of the header, the EXIF of the embedded JPEG (Make, Model,
    Orientation) and the RAF metadata directory tags 0x100 (RawImageFullSize), 0x110
    (RawImageCropTopLeft), 0x111 (RawImageCroppedSize) and 0x121 (RawImageSize), named as in
    ExifTool's FujiFilm RAF table (https://exiftool.org/TagNames/FujiFilm.html#RAF).

Usage:
    python tests/qualification/tools/file_facts.py [--manifest fixtures/manifest.json] [--out FILE]

Writes one JSON object keyed by fixture id (cached files only). The output is evidence for a
reviewer; it is not read by the C++ check suite.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

TIFF_TYPES = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8, 13: 4, 16: 8, 17: 8, 18: 8}
INTERESTING = {
    254: "NewSubFileType", 256: "ImageWidth", 257: "ImageLength", 258: "BitsPerSample", 259: "Compression",
    262: "PhotometricInterpretation", 271: "Make", 272: "Model", 274: "Orientation", 277: "SamplesPerPixel",
    305: "Software", 33421: "CFARepeatPatternDim", 33422: "CFAPattern", 41730: "ExifCFAPattern",
    50706: "DNGVersion", 50708: "UniqueCameraModel", 50710: "CFAPlaneColor", 50711: "CFALayout",
    50717: "WhiteLevel", 50719: "DefaultCropOrigin", 50720: "DefaultCropSize", 50829: "ActiveArea",
    51125: "DefaultUserCrop",
}
RW2_TAGS = {2: "SensorWidth", 3: "SensorHeight", 4: "SensorTopBorder", 5: "SensorLeftBorder",
            6: "SensorBottomBorder", 7: "SensorRightBorder", 9: "CFAPattern", 10: "BitsPerSample"}
CFA_NAMES = {0: "R", 1: "G", 2: "B", 3: "C", 4: "M", 5: "Y", 6: "W"}
CR3_CFA_LAYOUT = {0: "RGGB", 1: "GRBG", 2: "GBRG", 3: "BGGR"}
RW2_CFA = {1: "RGGB", 2: "GRBG", 3: "GBRG", 4: "BGGR"}  # ExifTool PanasonicRaw CFAPattern


class Reader:
    def __init__(self, data: bytes, little: bool):
        self.data = data
        self.e = "<" if little else ">"

    def u16(self, o: int) -> int:
        return struct.unpack_from(self.e + "H", self.data, o)[0]

    def u32(self, o: int) -> int:
        return struct.unpack_from(self.e + "I", self.data, o)[0]


def values(r: Reader, typ: int, count: int, off: int):
    if typ in (1, 7):
        return list(r.data[off:off + count])
    if typ == 2:
        return r.data[off:off + count].split(b"\0")[0].decode("latin-1").strip()
    if typ == 3:
        return [r.u16(off + 2 * i) for i in range(count)]
    if typ in (4, 13):
        return [r.u32(off + 4 * i) for i in range(count)]
    if typ in (5, 10):
        fmt = r.e + ("II" if typ == 5 else "ii")
        out = []
        for i in range(count):
            n, d = struct.unpack_from(fmt, r.data, off + 8 * i)
            out.append(n / d if d else None)
        return out
    if typ == 11:
        return [struct.unpack_from(r.e + "f", r.data, off + 4 * i)[0] for i in range(count)]
    if typ == 12:
        return [struct.unpack_from(r.e + "d", r.data, off + 8 * i)[0] for i in range(count)]
    return None


def walk_tiff(data: bytes, base: int, little: bool, first: int, rw2: bool = False) -> list[dict]:
    """Every IFD reachable from `first` (offsets relative to `base`), depth-first."""
    r = Reader(data, little)
    out: list[dict] = []
    seen: set[int] = set()

    def ifd(offset: int, path: str, depth: int) -> int:
        at = base + offset
        if offset == 0 or at in seen or at + 2 > len(data) or depth > 4 or len(out) > 64:
            return 0
        seen.add(at)
        n = r.u16(at)
        entry = {"path": path}
        subs: list[int] = []
        exif = 0
        for k in range(n):
            e = at + 2 + 12 * k
            if e + 12 > len(data):
                break
            tag, typ, count = r.u16(e), r.u16(e + 2), r.u32(e + 4)
            size = TIFF_TYPES.get(typ, 0) * count
            voff = e + 8 if size <= 4 else base + r.u32(e + 8)
            if size == 0 or voff + size > len(data):
                continue
            if tag == 330:
                subs = values(r, typ, count, voff)
            elif tag == 37500:  # MakerNote (Exif IFD): located for the readers below
                entry["MakerNote"] = {"offset": voff, "count": count}
            elif tag == 34665:
                exif = values(r, typ, count, voff)[0]
            elif rw2 and depth == 0 and tag in RW2_TAGS:
                entry[RW2_TAGS[tag]] = values(r, typ, count, voff)
            elif tag in INTERESTING and size <= 4096:
                entry[INTERESTING[tag]] = values(r, typ, count, voff)
        out.append(entry)
        for i, s in enumerate(subs or []):
            ifd(s, f"{path}.subifd{i}", depth + 1)
        if exif:
            ifd(exif, f"{path}.exif", depth + 1)
        nxt_at = at + 2 + 12 * n
        return r.u32(nxt_at) if nxt_at + 4 <= len(data) else 0

    offset, index = first, 0
    while offset and index < 16:
        offset = ifd(offset, f"ifd{index}", 0)
        index += 1
    return out


def read_ifd(data: bytes, little: bool, at: int, value_base: int) -> dict[int, object]:
    """Tag -> values of the IFD at absolute offset `at` (out-of-line values at value_base + offset)."""
    r = Reader(data, little)
    out: dict[int, object] = {}
    if at + 2 > len(data):
        return out
    n = r.u16(at)
    for k in range(min(n, 512)):
        e = at + 2 + 12 * k
        if e + 12 > len(data):
            break
        tag, typ, count = r.u16(e), r.u16(e + 2), r.u32(e + 4)
        size = TIFF_TYPES.get(typ, 0) * count
        voff = e + 8 if size <= 4 else value_base + r.u32(e + 8)
        if size == 0 or voff + size > len(data) or size > 65536:
            continue
        out[tag] = values(r, 4 if typ == 13 else typ, count, voff)
    return out


def canon_sensor_info(data: bytes, little: bool, maker_note: dict) -> dict | None:
    """Canon SensorInfo (maker note tag 0x00e0). Canon maker notes are a plain IFD whose values lie
    at offsets from the TIFF header."""
    tags = read_ifd(data, little, maker_note["offset"], 0)
    info = tags.get(0x00E0)
    if not isinstance(info, list) or len(info) < 9:
        return None
    left, top, right, bottom = info[5], info[6], info[7], info[8]
    return {"tag": "Canon SensorInfo (0x00e0)", "SensorWidth": info[1], "SensorHeight": info[2],
            "SensorLeftBorder": left, "SensorTopBorder": top, "SensorRightBorder": right,
            "SensorBottomBorder": bottom,
            "declared_area": {"x": left, "y": top, "width": right - left + 1, "height": bottom - top + 1}}


def olympus_crop(data: bytes, maker_note: dict) -> dict | None:
    """Olympus ImageProcessing crop (maker note tag 0x2040 -> tags 0x0612-0x0615). A maker note
    "OLYMPUS\0" + byte order + version holds an IFD at +12 whose offsets are relative to its start."""
    start = maker_note["offset"]
    head = data[start:start + 12]
    if not head.startswith(b"OLYMPUS\0") or head[8:10] not in (b"II", b"MM"):
        return None
    little = head[8:10] == b"II"
    tags = read_ifd(data, little, start + 12, start)
    sub = tags.get(0x2040)
    if not isinstance(sub, list) or not sub:
        return None
    ip = read_ifd(data, little, start + sub[0], start)
    names = {0x0612: "CropLeft", 0x0613: "CropTop", 0x0614: "CropWidth", 0x0615: "CropHeight"}
    crop = {name: ip[t][0] for t, name in names.items() if isinstance(ip.get(t), list) and ip[t]}
    if len(crop) != 4:
        return None
    return {"tag": "Olympus ImageProcessing (0x2040) 0x0612-0x0615", **crop,
            "declared_area": {"x": crop["CropLeft"], "y": crop["CropTop"], "width": crop["CropWidth"],
                              "height": crop["CropHeight"]}}


def tiff_header(data: bytes, base: int = 0):
    magic = data[base:base + 4]
    if magic[:2] == b"II":
        little = True
    elif magic[:2] == b"MM":
        little = False
    else:
        return None
    r = Reader(data, little)
    return little, r.u32(base + 4), magic


def cfa_text(dim, pattern) -> str | None:
    if not dim or not pattern:
        return None
    rows, cols = dim[0], dim[1]
    if len(pattern) != rows * cols:
        return None
    return "/".join("".join(CFA_NAMES.get(pattern[y * cols + x], "?") for x in range(cols)) for y in range(rows))


def exif_cfa_text(raw) -> str | None:
    # EXIF 2.32 CFAPattern (41730): SHORT horizontal repeat, SHORT vertical repeat, then colours.
    # The two SHORTs use the byte order of the file; try both and keep the plausible reading.
    if not isinstance(raw, list) or len(raw) < 6:
        return None
    for little in (True, False):
        h = raw[0] | raw[1] << 8 if little else raw[0] << 8 | raw[1]
        v = raw[2] | raw[3] << 8 if little else raw[2] << 8 | raw[3]
        if 1 <= h <= 8 and 1 <= v <= 8 and len(raw) == 4 + h * v:
            return cfa_text([v, h], raw[4:])
    return None


def summarise_tiff(ifds: list[dict]) -> dict:
    facts: dict = {"ifds": ifds}
    for i in ifds:
        for key in ("Make", "Model", "Orientation", "UniqueCameraModel", "DNGVersion"):
            if key in i and key not in facts:
                facts[key] = i[key]
        if "CFAPattern" in i:
            facts.setdefault("cfa_declared", []).append(
                {"ifd": i["path"], "tag": "CFAPattern (33422)", "pattern": cfa_text(i.get("CFARepeatPatternDim"), i["CFAPattern"])})
        if "ExifCFAPattern" in i:
            facts.setdefault("cfa_declared", []).append(
                {"ifd": i["path"], "tag": "EXIF CFAPattern (41730)", "pattern": exif_cfa_text(i["ExifCFAPattern"])})
    return facts


def iter_boxes(data: bytes, start: int, end: int):
    o = start
    while o + 8 <= end:
        size, typ = struct.unpack_from(">I4s", data, o)
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, o + 8)[0]
            header = 16
        elif size == 0:
            size = end - o
        if size < header or o + size > end:
            return
        yield typ, o, o + header, o + size
        o += size


CANON_UUID = bytes.fromhex("85c0b687820f11e08111f4ce462b6a48")


def cr3_facts(data: bytes) -> dict:
    facts: dict = {"cmp1": []}
    for typ, _, body, end in iter_boxes(data, 0, len(data)):
        if typ != b"moov":
            continue
        for t2, _, b2, e2 in iter_boxes(data, body, end):
            if t2 == b"uuid" and data[b2:b2 + 16] == CANON_UUID:
                for t3, _, b3, e3 in iter_boxes(data, b2 + 16, e2):
                    if t3 == b"CMT1":
                        hdr = tiff_header(data, b3)
                        if hdr:
                            facts.update({k: v for k, v in summarise_tiff(walk_tiff(data, b3, hdr[0], hdr[1])).items()
                                          if k != "ifds"})
        # CMP1 boxes sit inside each trak's CRAW sample entry; locate them by type and parse the
        # fixed layout documented by lclevy/canon_cr3.
        o = body
        while True:
            o = data.find(b"CMP1", o, end)
            if o < 4:
                break
            box = o - 4
            size = struct.unpack_from(">I", data, box)[0]
            if size >= 0x3C and box + size <= end:
                width, height, tw, th = struct.unpack_from(">IIII", data, box + 16)
                bits = data[box + 32]
                planes, layout = data[box + 33] >> 4, data[box + 33] & 15
                encoding, levels = data[box + 34] >> 4, data[box + 34] & 15
                facts["cmp1"].append({"image_width": width, "image_height": height, "tile_width": tw,
                                      "tile_height": th, "bits_per_sample": bits, "planes": planes,
                                      "cfa_layout": layout, "cfa": CR3_CFA_LAYOUT.get(layout),
                                      "encoding_type": encoding, "wavelet_levels": levels})
            o += 4
    return facts


def raf_facts(data: bytes) -> dict:
    facts: dict = {"camera_string": data[28:60].split(b"\0")[0].decode("latin-1")}
    jpeg_off, jpeg_len, dir_off = struct.unpack_from(">II", data, 84) + (struct.unpack_from(">I", data, 92)[0],)
    jpeg = data[jpeg_off:jpeg_off + jpeg_len]
    o = 2
    while o + 4 <= len(jpeg) and jpeg[o] == 0xFF:
        marker, length = jpeg[o + 1], struct.unpack_from(">H", jpeg, o + 2)[0]
        if marker == 0xE1 and jpeg[o + 4:o + 10] == b"Exif\0\0":
            base = o + 10
            hdr = tiff_header(jpeg, base)
            if hdr:
                facts.update({k: v for k, v in summarise_tiff(walk_tiff(jpeg, base, hdr[0], hdr[1])).items()
                              if k in ("Make", "Model", "Orientation")})
            break
        o += 2 + length
    count = struct.unpack_from(">I", data, dir_off)[0]
    p = dir_off + 4
    names = {0x100: "RawImageFullSize", 0x110: "RawImageCropTopLeft", 0x111: "RawImageCroppedSize",
             0x121: "RawImageSize"}
    for _ in range(min(count, 512)):
        tag, size = struct.unpack_from(">HH", data, p)
        if tag in names and size == 4:
            a, b = struct.unpack_from(">HH", data, p + 4)
            # The vertical value comes first: height then width for the sizes (RawImageSize 0x121
            # too), top then left for RawImageCropTopLeft.
            if tag == 0x110:
                facts[names[tag]] = {"top": a, "left": b}
            else:
                facts[names[tag]] = {"height": a, "width": b}
        p += 4 + size
    return facts


def file_facts(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] == b"\0\0\0\x18" or data[4:8] == b"ftyp":
        return {"container": "cr3", **cr3_facts(data)}
    if data[:8] == b"FUJIFILM":
        return {"container": "raf", **raf_facts(data)}
    hdr = tiff_header(data)
    if hdr is None:
        return {"container": "unknown"}
    little, first, magic = hdr
    rw2 = magic == b"IIU\0"
    kind = "rw2" if rw2 else "orf" if magic in (b"IIRO", b"IIRS", b"MMOR") else "tiff"
    facts = summarise_tiff(walk_tiff(data, 0, little, first, rw2=rw2))
    maker_note = next((i["MakerNote"] for i in facts["ifds"] if "MakerNote" in i), None)
    make = str(facts.get("Make", ""))
    if maker_note and make.startswith("Canon"):
        info = canon_sensor_info(data, little, maker_note)
        if info:
            facts["camera_crop_declared"] = info
    elif maker_note and make.startswith("OLYMPUS"):
        crop = olympus_crop(data, maker_note)
        if crop:
            facts["camera_crop_declared"] = crop
    if rw2:
        main = facts["ifds"][0]
        if "CFAPattern" in main:
            facts["cfa_declared"] = [{"ifd": "ifd0", "tag": "PanasonicRaw CFAPattern (9)",
                                      "pattern": RW2_CFA.get(main["CFAPattern"][0])}]
    if any("DNGVersion" in i for i in facts["ifds"]):
        kind = "dng"
    return {"container": kind, **facts}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    root = Path(__file__).resolve().parents[3]
    parser.add_argument("--manifest", type=Path, default=root / "fixtures" / "manifest.json")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    result = {}
    for fixture in manifest["fixtures"]:
        path = args.manifest.parent / "cache" / fixture["id"] / fixture["file_name"]
        if path.exists():
            result[fixture["id"]] = file_facts(path)
    text = json.dumps(result, indent=1, sort_keys=False, default=str)
    if args.out:
        args.out.write_text(text + "\n", encoding="utf-8")
    else:
        sys.stdout.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
