"""pxr Usd_IntegerCompression: delta coding + 2-bit width codes + LZ4."""

import struct
from collections import Counter

from . import lz4

# (common fmt, small, medium, large) for 32 / 64 bit
_W32 = ("<i", "b", "h", "i")
_W64 = ("<q", "h", "i", "q")
_SIZES = {"b": 1, "h": 2, "i": 4, "q": 8}


def _wrap(v, bits):
    m = 1 << bits
    v &= m - 1
    return v - m if v >= (m >> 1) else v


def encode(values, is64=False):
    bits = 64 if is64 else 32
    fmts = _W64 if is64 else _W32
    if not len(values):
        return b""
    deltas = []
    prev = 0
    for v in values:
        v = int(v)
        deltas.append(_wrap(v - prev, bits))
        prev = v
    n = len(deltas)
    common = Counter(deltas).most_common(1)[0][0] if n else 0
    codes = bytearray((n * 2 + 7) // 8)
    lim = [(1 << (_SIZES[f] * 8 - 1)) for f in fmts[1:3]]
    vparts = []
    for i, d in enumerate(deltas):
        if d == common:
            continue
        if -lim[0] <= d < lim[0]:
            c, f = 1, fmts[1]
        elif -lim[1] <= d < lim[1]:
            c, f = 2, fmts[2]
        else:
            c, f = 3, fmts[3]
        codes[i >> 2] |= c << ((i & 3) * 2)
        vparts.append(struct.pack("<" + f, d))
    return struct.pack(fmts[0], common) + bytes(codes) + b"".join(vparts)


def decode(buf, count, is64=False, signed=False):
    if count == 0:
        return []
    fmts = _W64 if is64 else _W32
    bits = 64 if is64 else 32
    csz = 8 if is64 else 4
    (common,) = struct.unpack_from(fmts[0], buf, 0)
    codes_off = csz
    vpos = codes_off + (count * 2 + 7) // 8
    structs = [None] + [struct.Struct("<" + f) for f in fmts[1:]]
    out = [0] * count
    prev = 0
    mask = (1 << bits) - 1
    for i in range(count):
        c = (buf[codes_off + (i >> 2)] >> ((i & 3) * 2)) & 3
        if c == 0:
            d = common
        else:
            s = structs[c]
            (d,) = s.unpack_from(buf, vpos)
            vpos += s.size
        prev = (prev + d) & mask
        out[i] = prev
    if signed:
        half = 1 << (bits - 1)
        out = [v - (1 << bits) if v >= half else v for v in out]
    return out


def compress_ints(values, is64=False):
    """[u64 compressed size][TfFastCompression(encode(values))]"""
    blob = lz4.compress(encode(values, is64))
    return struct.pack("<Q", len(blob)) + blob


def read_compressed_ints(buf, pos, count, is64=False, signed=False):
    """Read a compress_ints() block at pos. Returns (values, new_pos)."""
    (size,) = struct.unpack_from("<Q", buf, pos)
    pos += 8
    raw = lz4.decompress(buf[pos:pos + size])
    return decode(raw, count, is64, signed), pos + size
