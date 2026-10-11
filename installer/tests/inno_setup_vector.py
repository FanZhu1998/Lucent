r"""Builds the synthetic Inno Setup 6.7 executable of installer\tests\Test-BuilderPathScan.ps1 (base64 on stdout).

Run with Python 3 (only the standard library: lzma, zlib); paste the output into the test. The
encoders are liblzma's and zlib's, independent of the decoders under test (W12 review SEC-07).
The output is deterministic.

Layout (as ISCC 6.7.3 writes it; installer\scripts\LucentCompressedContent.cs reads it):
  "MZ" loader stand-in with SetupLdr's offset table (version 2) at 0x40
  setup-1 at Offset1: "zlb" #26, LZMA2 property byte, LZMA2 chunks (the installed files)
  setup-0 at Offset0: 64-byte identifier, CRC-32 + 49-byte encryption header, then blocks
           (CRC-32 of [int64 stored size, compressed flag], chunks of CRC-32 + up to 4096 bytes,
           LZMA1 with 5 property bytes and an end marker)
  the setup engine block at OffsetEXE (call-transformed; the table holds its size and CRC-32)
Planted builder paths (made-up machine of the test): an ASCII path in the installed files, a UTF-16
path in the setup data, none in the engine.
"""
import base64
import lzma
import struct
import zlib


def lzma1_with_props(data):
    alone = lzma.compress(data, format=lzma.FORMAT_ALONE, filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16, "lc": 3, "lp": 0, "pb": 2}])
    return alone[:5] + alone[13:]  # properties and dictionary size, no 8-byte size


def lzma2_stream(data):
    raw = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 1 << 16}])
    return bytes([0x10]) + raw


def block(payload, compressed=True):
    body = lzma1_with_props(payload) if compressed else payload
    chunks = bytearray()
    for i in range(0, len(body), 4096):
        piece = body[i:i + 4096]
        chunks += struct.pack("<I", zlib.crc32(piece) & 0xFFFFFFFF) + piece
    head = struct.pack("<qB", len(chunks), 1 if compressed else 0)
    return struct.pack("<I", zlib.crc32(head) & 0xFFFFFFFF) + head + bytes(chunks)


def call_transform(buffer):
    """Inno Setup's encoding of x86 CALL/JMP addresses (the inverse of UndoCallTransform)."""
    b = bytearray(buffer)
    size = len(b) - 4
    i = 0
    while i < size:
        if b[i] not in (0xE8, 0xE9):
            i += 1
            continue
        i += 1
        if b[i + 3] in (0x00, 0xFF):
            rel = b[i] | (b[i + 1] << 8) | (b[i + 2] << 16)
            addr = (rel + i + 4) & 0xFFFFFF
            b[i], b[i + 1], b[i + 2] = addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF
            if rel & 0x800000:
                b[i + 3] ^= 0xFF
        i += 4
    return bytes(b)


files = (b"MZ fake installed program " * 40 + b"\x00" +
         b"D:\\src\\Lucent\\build\\release\\vcpkg_installed\\x64-windows-lucent\\bin\\planted.dll" + b"\x00" +
         b"more file data " * 60)
header = ("Lucent" * 30 + "\0" + "C:\\Users\\builder\\AppData\\Local\\Temp\\planted.iss" + "\0" + "messages " * 50).encode("utf-16-le")
locations = b"\x01\x00\x00\x00" * 8
engine_plain = bytearray(b"MZ engine " * 50)
# x86 calls and jumps with small positive and negative relative addresses.
for k, (opcode, rel, high) in enumerate([(0xE8, 0x001234, 0x00), (0xE9, 0xFFF000, 0xFF), (0xE8, 0x000010, 0x00)]):
    at = 100 + 37 * k
    engine_plain[at:at + 5] = bytes([opcode, rel & 0xFF, (rel >> 8) & 0xFF, (rel >> 16) & 0xFF, high])
engine_plain = bytes(engine_plain)
engine_stored = call_transform(engine_plain)

loader = bytearray(b"MZ" + b"\x00" * 0x3E)
table_at = len(loader)
loader += b"\x00" * 64  # the table, filled in below
loader += b"loader code " * 8
offset1 = len(loader)
setup1 = b"zlb\x1a" + lzma2_stream(files)
offset0 = offset1 + len(setup1)
ident = b"Inno Setup Setup Data (6.7.0)".ljust(64, b"\x00")
encryption = bytes(17) + struct.pack("<I", 220000) + bytes(28)
setup0 = ident + struct.pack("<I", zlib.crc32(encryption) & 0xFFFFFFFF) + encryption + block(header) + block(locations)
offset_exe = offset0 + len(setup0)
engine_block = block(engine_stored)
total = offset_exe + len(engine_block)
table = bytearray(b"rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a")
table += struct.pack("<I", 2) + struct.pack("<q", total) + struct.pack("<q", offset_exe)
table += struct.pack("<II", len(engine_plain), zlib.crc32(engine_plain) & 0xFFFFFFFF)
table += struct.pack("<qq", offset0, offset1) + struct.pack("<I", 0)
table += struct.pack("<I", zlib.crc32(bytes(table)) & 0xFFFFFFFF)
assert len(table) == 64
loader[table_at:table_at + 64] = table
exe = bytes(loader) + setup1 + setup0 + engine_block
assert len(exe) == total
print(base64.b64encode(exe).decode())
