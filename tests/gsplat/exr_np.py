# SPDX-License-Identifier: Apache-2.0
"""Minimal OpenEXR reader (numpy only) for test comparisons.

Supports single-part scanline images with NONE, ZIPS or ZIP compression and
HALF/FLOAT channels: what lusdrender (tinyexr) and Blender write by default.
read_rgb(path) -> float64 array (h, w, 3), top row first.
"""
import struct
import zlib

import numpy as np

_NONE, _RLE, _ZIPS, _ZIP = 0, 1, 2, 3
_LINES = {_NONE: 1, _ZIPS: 1, _ZIP: 16}
_DT = {1: np.float16, 2: np.float32, 0: np.uint32}


def _attrs(buf, pos):
    out = {}
    while buf[pos] != 0:
        end = buf.index(b"\0", pos)
        name = buf[pos:end].decode()
        pos = end + 1
        end = buf.index(b"\0", pos)
        typ = buf[pos:end].decode()
        pos = end + 1
        (size,) = struct.unpack_from("<i", buf, pos)
        pos += 4
        out[name] = (typ, buf[pos:pos + size])
        pos += size
    return out, pos + 1


def _channels(data):
    chans, pos = [], 0
    while data[pos] != 0:
        end = data.index(b"\0", pos)
        name = data[pos:end].decode()
        ptype, _, _, _ = struct.unpack_from("<iBBBBii", data, end + 1)[:4]
        chans.append((name, ptype))
        pos = end + 1 + 16
    return chans


def _unzip(raw):
    d = np.frombuffer(zlib.decompress(raw), dtype=np.uint8).astype(np.int64)
    # Undo the predictor: t[0] = d[0], t[i] = t[i-1] + d[i] - 128 (mod 256).
    d[1:] -= 128
    t = (np.cumsum(d) % 256).astype(np.uint8)
    # Undo the interleave: first half holds even bytes, second half odd.
    half = (len(t) + 1) // 2
    out = np.empty_like(t)
    out[0::2] = t[:half]
    out[1::2] = t[half:]
    return out.tobytes()


def read_rgb(path):
    buf = open(path, "rb").read()
    if struct.unpack_from("<i", buf, 0)[0] != 20000630:
        raise ValueError("%s: not an OpenEXR file" % path)
    if struct.unpack_from("<i", buf, 4)[0] & 0x1a00:
        raise ValueError("%s: tiled/deep/multipart EXR not supported" % path)
    attrs, pos = _attrs(buf, 8)
    comp = attrs["compression"][1][0]
    if comp not in _LINES:
        raise ValueError("%s: unsupported EXR compression %d" % (path, comp))
    x0, y0, x1, y1 = struct.unpack("<iiii", attrs["dataWindow"][1])
    w, h = x1 - x0 + 1, y1 - y0 + 1
    chans = _channels(attrs["channels"][1])  # stored sorted by name
    sizes = [np.dtype(_DT[t]).itemsize for _, t in chans]
    lines = _LINES[comp]
    nblocks = (h + lines - 1) // lines
    offsets = struct.unpack_from("<%dQ" % nblocks, buf, pos)
    planes = {name: np.zeros((h, w), np.float64) for name, _ in chans}
    for off in offsets:
        y, size = struct.unpack_from("<ii", buf, off)
        raw = buf[off + 8:off + 8 + size]
        n = min(lines, y1 - y + 1)
        expect = n * w * sum(sizes)
        data = raw if (comp == _NONE or len(raw) == expect) else _unzip(raw)
        p = 0
        for row in range(n):
            for (name, t), s in zip(chans, sizes):
                a = np.frombuffer(data, dtype=_DT[t], count=w, offset=p)
                planes[name][y - y0 + row] = a
                p += w * s
    get = lambda c: planes.get(c, planes.get(c.lower()))
    return np.stack([get("R"), get("G"), get("B")], axis=-1)
