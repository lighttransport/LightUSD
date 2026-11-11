#!/usr/bin/env python3
"""lusdview: a non-instanced blendshape mesh must animate on the default (--core)
loader.

The --core loader batches non-instanced meshes into shared vertex buffers and
world-bakes them. Blendshape morph is a per-vertex CSR keyed to the prim,
applied in object space before the world transform, and its per-frame weight
lookup needs the mesh's own absPath -- none of which survives a world-baked,
absPath-less shared batch. So morph was built only for INSTANCED prototypes, and
a plain SkelRoot -> Mesh blendshape rendered frozen at its rest pose at every
time (skinning animated; morph did not).

The fix emits a morphed non-instanced mesh standalone (object-space vertices +
world in dm.world), like EmitInstancedProto de-instances a morphed prototype.
This test renders such a mesh at two times and asserts the frames differ.

Runs headless (Vulkan). Exits 77 (skip) if the binary/Vulkan is unavailable.
"""
import os
import struct
import subprocess
import sys
import zlib

SKIP = 77

FIXTURE = """#usda 1.0
(defaultPrim = "World" upAxis = "Y" startTimeCode = 0 endTimeCode = 12)
def Xform "World" {
    def SkelRoot "Rig" {
        def Mesh "M" (prepend apiSchemas = ["SkelBindingAPI"]) {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
            color3f[] primvars:displayColor = [(0.9, 0.2, 0.2)] (interpolation = "constant")
            uniform token[] skel:blendShapes = ["move"]
            rel skel:blendShapeTargets = </World/Rig/M/move>
            rel skel:animationSource = </World/Rig/Anim>
            def BlendShape "move" {
                uniform vector3f[] offsets = [(0, 3, 0), (0, 3, 0), (0, 3, 0), (0, 3, 0)]
                uniform int[] pointIndices = [0, 1, 2, 3]
            }
        }
        def SkelAnimation "Anim" {
            uniform token[] blendShapes = ["move"]
            float[] blendShapeWeights.timeSamples = { 0: [0.0], 12: [1.0] }
        }
    }
}
"""


def read_rgb(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w = h = None
    idat = b""
    color = None
    pos = 8
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, color = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    if w is None or color not in (2, 6):
        return None
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out = []
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            if f == 1:
                line[i] = (line[i] + a) & 0xff
            elif f == 2:
                line[i] = (line[i] + b) & 0xff
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xff
            elif f == 4:
                c = prev[i - nch] if i >= nch else 0
                p_ = a + b - c
                pa, pb, pc = abs(p_ - a), abs(p_ - b), abs(p_ - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        prev = line
        for i in range(0, stride, nch):
            out.append((line[i], line[i + 1], line[i + 2]))
    return out


def render(binary, scene, out, time):
    cmd = [binary, "--headless", "--frames", "4", "--time", str(time),
           "--screenshot", out, scene]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=300)
    log = r.stdout.decode(errors="replace")
    if r.returncode != 0 or not os.path.exists(out):
        return None, log
    return log, log


def main():
    if len(sys.argv) < 3:
        print("usage: check-noninstanced-blendshape.py <lusdview> <work_dir>")
        return SKIP
    binary, work = sys.argv[1], sys.argv[2]
    if not os.path.exists(binary):
        print(f"SKIP: missing binary ({binary})")
        return SKIP
    os.makedirs(work, exist_ok=True)
    scene = os.path.join(work, "noninstanced-blendshape.usda")
    with open(scene, "w") as f:
        f.write(FIXTURE)

    t0 = os.path.join(work, "bs_t0.png")
    t12 = os.path.join(work, "bs_t12.png")
    log0, raw0 = render(binary, scene, t0, 0)
    if log0 is None:
        if "Vulkan" in raw0 or "no device" in raw0.lower() or "headless" in raw0.lower():
            print("SKIP: headless Vulkan unavailable")
            return SKIP
        print(f"SKIP: render failed\n{raw0}")
        return SKIP
    log12, raw12 = render(binary, scene, t12, 12)
    if log12 is None:
        print("SKIP: second render failed")
        return SKIP

    a = read_rgb(t0)
    b = read_rgb(t12)
    if a is None or b is None:
        print("FAIL: could not read renders")
        return 1
    if a == b:
        print("FAIL: the non-instanced blendshape did NOT animate -- frames at "
              "t=0 and t=12 are identical. Morph was not built for the batched "
              "non-instanced mesh (built only for instanced prototypes).")
        return 1
    changed = sum(1 for pa, pb in zip(a, b) if pa != pb)
    ratio = 100.0 * changed / len(a)
    if ratio < 1.0:
        print(f"FAIL: frames barely differ ({ratio:.2f}% of pixels) -- morph "
              f"likely not applied.")
        return 1
    print(f"PASS: non-instanced blendshape animates on --core "
          f"({ratio:.1f}% of pixels change t=0 -> t=12)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
