#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""lusdrender smoke test of a skinned OBJ (skeleton + FBX animation sidecar).

  render_smoke.py LUSDRENDER WORKDIR

Renders tests/obj-skel/obj_scene.usda at time codes 1 and 12 (rest pose and
the middle of the bend). Both renders must succeed, be non-blank, and differ.
"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def pixels(path):
    d = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        if t == b"IDAT":
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    return zlib.decompress(idat)


def main():
    exe, work = sys.argv[1:3]
    os.makedirs(work, exist_ok=True)
    imgs = []
    for t in (1, 12):
        out = os.path.join(work, f"obj_t{t}.png")
        r = subprocess.run([exe, os.path.join(HERE, "obj_scene.usda"), out, "-timecode", str(t),
                            "-width", "128", "-height", "128"], capture_output=True, text=True)
        if r.returncode or not os.path.exists(out):
            print(f"FAIL render t={t}: {r.stderr[-400:]}")
            return 1
        imgs.append(pixels(out))
    blank = [len(set(p)) <= 2 for p in imgs]
    differ = sum(a != b for a, b in zip(*imgs))
    print(f"non-blank: {not any(blank)}, differing bytes: {differ}")
    return 0 if not any(blank) and differ > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
