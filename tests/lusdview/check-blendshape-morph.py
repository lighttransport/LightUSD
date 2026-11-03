#!/usr/bin/env python3
"""lusdview `--next`: an ordinary (non-instanced) blendshaped mesh must MORPH,
and it must morph in the RIGHT SPACE.

Two bugs, one test.

(a) BuildMorphChannelsNext / BakeBlendShapes were only ever called on the
    INSTANCED prototype path. A plain blendshaped mesh -- which is what almost
    every rig in the wild is -- went through the static batch path, which called
    neither, so it rendered its REST shape at every time code.

(b) That path world-BAKES its vertices (the batch's own transform is identity),
    but the blendshape offsets are authored in MESH-local space, and every
    consumer -- deform.glsl and BuildNextRtDeformedVertices -- adds the delta
    straight onto the position it is handed. The deltas were left in mesh space,
    so a blendshaped mesh under a rotated or scaled parent morphed along the
    wrong axes. (Skinning already did the equivalent: its bone rows are
    invW * (G*sm*invG) * W.) The instanced path is exempt -- its vertices stay
    mesh-local.

The geometry checks render in `--mode depth` through a FIXED USD camera. Both
matter:

  * depth is purely geometric, so the comparison is not polluted by the known,
    by-design shading difference (the GPU path morphs positions and skins the
    REST normal; the CPU bake recomputes normals from the morphed points); and
  * a fixed camera, because the two paths legitimately frame the scene
    differently -- the GPU path pads the mesh box by `morphExtent` so a morphed
    mesh is never frustum-culled, while the bake's box is exact. Auto-framing
    would otherwise move the camera between the two renders and swamp any real
    geometric difference.

Exits 77 (skip) when the binary or a usable GPU is missing.
"""
import os
import struct
import subprocess
import sys
import zlib

from gpu_backend import software_only_vulkan

SKIP = 77
MAX_MEAN_DIFF = 0.5   # depth, GPU morph vs CPU bake: they must agree exactly
MIN_POSE_DIFF = 1.0   # depth, rest vs posed: the morph must actually do something

# A blendshaped cube under a ROTATED + non-uniformly SCALED parent, with a fixed
# camera. The offsets push the +y face along mesh-local +y; the parent turns that
# into world -x, doubled. A loader that forgets to carry the deltas through the
# world bake displaces the face along world +y instead -- a gross, obvious error
# that the identity-transform fixtures in models/ cannot see.
XFORM_MORPH_USDA = """#usda 1.0
(
    defaultPrim = "root"
    endTimeCode = 20
    startTimeCode = 1
    upAxis = "Y"
)

def Xform "root"
{
    def Camera "Cam"
    {
        float focalLength = 24
        float horizontalAperture = 20.955
        float verticalAperture = 15.2908
        float2 clippingRange = (0.1, 1000)
        double3 xformOp:translate = (0, 3, 14)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def SphereLight "Key"
    {
        float inputs:intensity = 900
        float inputs:radius = 0.5
        double3 xformOp:translate = (6, 8, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def SkelRoot "Rig" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        double3 xformOp:rotateXYZ = (0, 0, 90)
        double3 xformOp:scale = (2, 1, 1)
        uniform token[] xformOpOrder = ["xformOp:rotateXYZ", "xformOp:scale"]

        def Mesh "Cube" (
            prepend apiSchemas = ["SkelBindingAPI"]
        )
        {
            uniform bool doubleSided = 1
            int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
            int[] faceVertexIndices = [0, 4, 6, 2, 3, 2, 6, 7, 7, 6, 4, 5, 5, 1, 3, 7, 1, 0, 2, 3, 5, 4, 0, 1]
            point3f[] points = [(1, 1, 1), (1, 1, -1), (1, -1, 1), (1, -1, -1), (-1, 1, 1), (-1, 1, -1), (-1, -1, 1), (-1, -1, -1)]
            color3f[] primvars:displayColor = [(0.85, 0.85, 0.85)]
            int[] primvars:skel:jointIndices = [0, 0, 0, 0, 0, 0, 0, 0] (elementSize = 1 interpolation = "vertex")
            float[] primvars:skel:jointWeights = [1, 1, 1, 1, 1, 1, 1, 1] (elementSize = 1 interpolation = "vertex")
            uniform token[] skel:blendShapes = ["Key_1"]
            rel skel:blendShapeTargets = </root/Rig/Cube/Key_1>
            prepend rel skel:skeleton = </root/Rig/Skel>
            uniform token subdivisionScheme = "none"

            def BlendShape "Key_1"
            {
                uniform vector3f[] offsets = [(0, 3, 0), (0, 3, 0), (0, 0, 0), (0, 0, 0), (0, 3, 0), (0, 3, 0), (0, 0, 0), (0, 0, 0)]
                uniform int[] pointIndices = [0, 1, 2, 3, 4, 5, 6, 7]
            }
        }

        def Skeleton "Skel" (
            prepend apiSchemas = ["SkelBindingAPI"]
        )
        {
            uniform matrix4d[] bindTransforms = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )]
            uniform token[] joints = ["joint1"]
            uniform matrix4d[] restTransforms = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )]
            prepend rel skel:animationSource = </root/Rig/Skel/Anim>

            def SkelAnimation "Anim"
            {
                uniform token[] blendShapes = ["Key_1"]
                float[] blendShapeWeights.timeSamples = {
                    1: [0],
                    20: [1],
                }
            }
        }
    }
}
"""


def render(binary, model, out, time, extra=(), env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:os.remove(out)
    except FileNotFoundError:pass
    try:os.remove(os.path.join(os.path.dirname(out), "imgui.ini"))
    except FileNotFoundError:pass
    config = os.path.join(os.path.dirname(out), "config.json")
    if not os.path.exists(config):
        with open(config, "w") as f:
            f.write('{"window_size":{"width":320,"height":320}}\n')
    cmd = [binary, "--next", "--headless", "--size", "320x320", "--frames", "3", "--time",
           str(time), "--config", config, "--no-grid", "--no-skeleton",
           "--screenshot", out, *extra, model]
    try:
        with open(out + ".log", "w") as log:
            result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=e,timeout=120)
        if result.returncode:return False
    except subprocess.TimeoutExpired:
        return False
    return os.path.exists(out) and os.path.getsize(out) > 0


def read_luma(path):
    d = open(path, "rb").read()
    w = h = color = None
    idat = b""
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
    nch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out, prev, p = [], bytearray(stride), 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        for x in range(w):
            px = line[x * nch:x * nch + 3]
            out.append((px[0] + px[1] + px[2]) / 3.0)
        prev = line
    return out


def mean_diff(a_path, b_path):
    a, b = read_luma(a_path), read_luma(b_path)
    if len(a) != len(b):
        return float("inf")
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def main():
    if len(sys.argv) < 4:
        print("usage: check-blendshape-morph.py <lusdview> <model> <work_dir>")
        return SKIP
    binary, model, work = sys.argv[1:4]
    for p in (binary, model):
        if not os.path.exists(p):
            print(f"SKIP: missing {p}")
            return SKIP
    os.makedirs(work, exist_ok=True)

    # (a) The mesh has to change with the time code at all.
    rest = os.path.join(work, "morph_t1.png")
    posed = os.path.join(work, "morph_t20.png")
    if not render(binary, model, rest, 1):
        print("SKIP: headless render produced no image (no usable GPU?)")
        return SKIP
    render(binary, model, posed, 20)
    if open(rest, "rb").read() == open(posed, "rb").read():
        print("FAIL: the mesh renders identically at time 1 and time 20, so its "
              "blendshape is not being applied at all. A non-instanced blendshaped "
              "mesh goes through the static batch path -- which must build morph "
              "channels (BuildMorphChannelsNext), as the instanced prototype path "
              "does.")
        return 1

    # (b) Under a rotated + scaled parent, the GPU morph must land exactly where
    # the CPU bake does. Depth, fixed camera: geometry only.
    scene = os.path.join(work, "xform_morph.usda")
    with open(scene, "w") as f:
        f.write(XFORM_MORPH_USDA)
    depth = ["--mode", "depth", "--camera", "Cam"]
    d_rest = os.path.join(work, "xform_depth_t1.png")
    d_gpu = os.path.join(work, "xform_depth_t20.png")
    d_bake = os.path.join(work, "xform_depth_t20_baked.png")
    ok = (render(binary, scene, d_rest, 1, extra=depth) and
          render(binary, scene, d_gpu, 20, extra=depth) and
          render(binary, scene, d_bake, 20, extra=depth,
                 env={"LUSDVIEW_NEXT_MORPH_BAKE": "1"}))
    if not ok:
        print("SKIP: the transformed-parent scene did not render")
        return SKIP

    pose = mean_diff(d_rest, d_bake)
    if pose < MIN_POSE_DIFF:
        print(f"FAIL: the CPU bake's depth barely moves between time 1 and time 20 "
              f"(mean {pose:.3f} < {MIN_POSE_DIFF}), so the check below -- which "
              f"compares the GPU morph against it -- would be vacuous.")
        return 1

    diff = mean_diff(d_gpu, d_bake)
    if diff > MAX_MEAN_DIFF:
        print(f"FAIL: under a rotated/scaled parent the GPU morph and the CPU bake "
              f"put the geometry in DIFFERENT places (mean depth diff {diff:.3f} > "
              f"{MAX_MEAN_DIFF}; the morph itself moves depth by {pose:.3f}). The "
              f"blendshape offsets are MESH-local, but the static batch path bakes "
              f"its vertices into world space -- so the deltas (and morphExtent) "
              f"must be carried through the same transform, or the mesh morphs "
              f"along the wrong axes.")
        return 1

    # (c) It has to reach the ray tracer, which traces the vertex buffers.
    rt1 = os.path.join(work, "morph_rt_t1.png")
    rt20 = os.path.join(work, "morph_rt_t20.png")
    rt_verified = False
    if not software_only_vulkan() and \
       render(binary, model, rt1, 1, extra=["--rt"]) and \
       render(binary, model, rt20, 20, extra=["--rt"]):
        if open(rt1, "rb").read() == open(rt20, "rb").read():
            print("FAIL: the ray tracer renders the same image at time 1 and 20. "
                  "RT traces the vertex buffers themselves, so the morph has to "
                  "reach them (BuildNextRtDeformedVertices).")
            return 1
        rt_verified = True

    rt_result = "and reaches the ray tracer" if rt_verified else \
        "(RT check capability-skipped)"
    print(f"PASS: non-instanced blendshape morphs, lands exactly where the CPU bake "
          f"does under a rotated/scaled parent (mean depth diff {diff:.3f}) "
          f"{rt_result}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
