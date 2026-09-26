"""Pure-python LZ4 block codec + pxr TfFastCompression framing."""

import struct


def block_decompress(src, max_size=None):
    src = memoryview(src)
    n = len(src)
    dst = bytearray()
    i = 0
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if lit:
            dst += src[i:i + lit]
            i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if off == 0 or off > len(dst):
            raise ValueError("lz4: invalid match offset")
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            pat = bytes(dst[start:])
            dst += (pat * (ml // off + 1))[:ml]
        if max_size is not None and len(dst) > max_size:
            raise ValueError("lz4: output exceeds expected size")
    return bytes(dst)


def _write_len(out, v):
    while v >= 255:
        out.append(255)
        v -= 255
    out.append(v)


def block_compress(src):
    """Greedy LZ4 block compressor (valid, not maximal ratio)."""
    src = bytes(src)
    n = len(src)
    out = bytearray()
    anchor = 0
    if n >= 13:
        table = {}
        i = 0
        limit = n - 12            # MFLIMIT
        match_end_limit = n - 5   # LASTLITERALS
        while i < limit:
            key = src[i:i + 4]
            ref = table.get(key)
            table[key] = i
            if ref is None or i - ref > 65535:
                i += 1
                continue
            ml = 4
            maxml = match_end_limit - i
            # extend in 8-byte strides, then bytewise
            while ml + 8 <= maxml and src[ref + ml:ref + ml + 8] == src[i + ml:i + ml + 8]:
                ml += 8
            while ml < maxml and src[ref + ml] == src[i + ml]:
                ml += 1
            litlen = i - anchor
            token = (min(litlen, 15) << 4) | min(ml - 4, 15)
            out.append(token)
            if litlen >= 15:
                _write_len(out, litlen - 15)
            out += src[anchor:i]
            out += struct.pack("<H", i - ref)
            if ml - 4 >= 15:
                _write_len(out, ml - 4 - 15)
            i += ml
            anchor = i
    litlen = n - anchor
    out.append(min(litlen, 15) << 4)
    if litlen >= 15:
        _write_len(out, litlen - 15)
    out += src[anchor:]
    return bytes(out)


def compress(data):
    """TfFastCompression::CompressToBuffer: [u8 nchunks=0][lz4 block]."""
    return b"\x00" + block_compress(data)


def decompress(data, max_size=None):
    data = memoryview(data)
    nchunks = data[0]
    if nchunks == 0:
        return block_decompress(data[1:], max_size)
    out = bytearray()
    pos = 1
    for _ in range(nchunks):
        (sz,) = struct.unpack_from("<i", data, pos)
        pos += 4
        out += block_decompress(data[pos:pos + sz])
        pos += sz
    return bytes(out)
