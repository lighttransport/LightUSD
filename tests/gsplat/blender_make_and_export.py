# SPDX-License-Identifier: Apache-2.0
"""Blender 5.3: create Gaussian splats natively, export them, render Cycles refs.

blender -b --factory-startup --python blender_make_and_export.py -- \
    OUT_DIR [--count N] [--seed S] [--width W] [--height H] [--spp N]
    [--degree 0-3] [--scale X,Y,Z] [--ortho] [--world R,G,B] [--exr]

Variants: --degree sets the SH degree; --scale gives a non-uniform object
scale (not representable in a transform-free PLY, so the PLY outputs and
ref_from_ply are skipped); --ortho uses an orthographic camera; --world sets
a constant world background colour (strength 1).

Writes into OUT_DIR:
  splats.usda        OpenUSD ParticleField3DGaussianSplat + camera (USD friendly)
  splats.ply         reference 3DGS PLY, world space (portable)
  scene_ply.usda     camera + reference to @splats.ply@ (usdPly path)
  blender_native.usda  Blender's own USD export, for comparison (lossy: it
                     writes a Points prim and drops radiance:base, i.e. the
                     DC colour and opacity)
  ref_seed0, ref_seed7 (.png, or .exr with --exr)   Cycles renders of the
                     native splat object (two seeds give the noise floor)
  ref_from_ply       Cycles render after re-importing splats.ply into Blender
"""
import argparse
import math
import os
import random
import struct
import sys

import bpy
import numpy as np  # bundled with Blender
from mathutils import Matrix, Quaternion, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--count", type=int, default=400)
ap.add_argument("--seed", type=int, default=3)
ap.add_argument("--width", type=int, default=320)
ap.add_argument("--height", type=int, default=180)
ap.add_argument("--spp", type=int, default=256)
ap.add_argument("--degree", type=int, default=3, choices=[0, 1, 2, 3])
ap.add_argument("--scale", default="1.3,1.3,1.3")
ap.add_argument("--ortho", action="store_true")
ap.add_argument("--world", default="0,0,0")
ap.add_argument("--exr", action="store_true",
                help="write references as 32-bit scene-linear EXR (no view transform)")
a = ap.parse_args(argv)
os.makedirs(a.out, exist_ok=True)
DEGREE = a.degree
OBJ_SCALE = tuple(float(v) for v in a.scale.split(","))
WORLD = tuple(float(v) for v in a.world.split(","))
UNIFORM = max(OBJ_SCALE) - min(OBJ_SCALE) < 1e-9
NCOEF = (DEGREE + 1) ** 2

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
rng = random.Random(a.seed)

# ---- splats, created natively as a Gaussian splat point cloud --------------
n = a.count
pos, base, scl, rot, rest = [], [], [], [], [[] for _ in range(NCOEF - 1)]
for i in range(n):
    if i < n // 2:  # structured ring of flat, tangent-aligned splats
        t = 2 * math.pi * i / (n // 2)
        p = (0.6 * math.cos(t), 0.6 * math.sin(t), 0.15 * math.sin(3 * t))
        q = Quaternion((0, 0, 1), t)
        s = (0.09, 0.035, 0.008)
    else:  # random anisotropic cloud
        p = (rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), rng.uniform(-0.3, 0.3))
        q = Quaternion([rng.gauss(0, 1) for _ in range(4)]).normalized()
        s = (rng.uniform(0.02, 0.12), rng.uniform(0.02, 0.12), rng.uniform(0.005, 0.06))
    o = rng.uniform(0.3, 0.97)
    dc = (rng.uniform(-1.5, 1.5), rng.uniform(-1.5, 1.5), rng.uniform(-1.5, 1.5))
    pos.append(p); rot.append(tuple(q)); scl.append(s); base.append(dc + (o,))
    for k in range(NCOEF - 1):
        rest[k].append(tuple(rng.uniform(-0.35, 0.35) for _ in range(3)))

pc = bpy.data.pointclouds.new("Splats")
pc.resize(n)
pc.type = "GAUSSIAN_SPLAT"


def put(name, dtype, values, width):
    attr = pc.attributes.get(name) or pc.attributes.new(name, dtype, "POINT")
    flat = [c for v in values for c in v]
    key = "value" if dtype in ("FLOAT4", "QUATERNION") else "vector"
    try:
        attr.data.foreach_set(key, flat)
    except (TypeError, AttributeError):
        attr.data.foreach_set("vector" if key == "value" else "value", flat)


put("position", "FLOAT_VECTOR", pos, 3)
put("radiance:base", "FLOAT4", base, 4)
put("scale", "FLOAT_VECTOR", scl, 3)
put("rotation", "QUATERNION", rot, 4)
for k in range(NCOEF - 1):
    put("radiance:sh_%d" % k, "FLOAT_VECTOR", rest[k], 3)

obj = bpy.data.objects.new("Splats", pc)
scene.collection.objects.link(obj)
# Rotation + uniform scale + translation: exactly representable after baking
# into the portable (transform-free) PLY.
obj.location = (0.1, -0.05, 0.2)
obj.rotation_euler = (0.4, -0.3, 0.7)
obj.scale = OBJ_SCALE

cam_data = bpy.data.cameras.new("Cam")
cam_data.lens = 40.0
cam_data.sensor_fit = "HORIZONTAL"
cam_data.sensor_width = 36.0
cam_data.clip_start = 0.01
cam_data.clip_end = 1000.0
if a.ortho:
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = 2.6
cam = bpy.data.objects.new("Cam", cam_data)
cam.location = (2.2, -2.6, 1.6)
scene.collection.objects.link(cam)
direction = Vector((0, 0, 0)) - cam.location
cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
scene.camera = cam
bpy.context.view_layer.update()

# ---- exports -----------------------------------------------------------------
M = obj.matrix_world.copy()
loc, qobj, sobj = M.decompose()
uniform = sobj[0]


def usd_matrix(m):  # Blender column-vector -> USD row-vector (transpose)
    return "( " + ", ".join(
        "(%r, %r, %r, %r)" % tuple(m[r][c] for r in range(4)) for c in range(4)) + " )"


f3 = lambda v: "(%r, %r, %r)" % tuple(v)
if a.ortho:
    # USD apertures are in tenths of a scene unit for orthographic cameras.
    aperture_h = cam_data.ortho_scale * 10.0
    cam_block = (
        '    def Camera "Cam"\n    {\n'
        '        token projection = "orthographic"\n'
        "        float horizontalAperture = %r\n        float verticalAperture = %r\n"
        "        float2 clippingRange = (0.01, 1000)\n"
        "        matrix4d xformOp:transform = %s\n"
        '        uniform token[] xformOpOrder = ["xformOp:transform"]\n    }\n'
    ) % (aperture_h, aperture_h * a.height / a.width, usd_matrix(cam.matrix_world))
else:
    cam_block = (
        '    def Camera "Cam"\n    {\n'
        "        float focalLength = %r\n        float horizontalAperture = %r\n"
        "        float verticalAperture = %r\n        float2 clippingRange = (0.01, 1000)\n"
        "        matrix4d xformOp:transform = %s\n"
        '        uniform token[] xformOpOrder = ["xformOp:transform"]\n    }\n'
    ) % (cam_data.lens, cam_data.sensor_width,
         cam_data.sensor_width * a.height / a.width, usd_matrix(cam.matrix_world))
header = '#usda 1.0\n(\n    defaultPrim = "World"\n    metersPerUnit = 1\n    upAxis = "Z"\n)\n\n'

with open(os.path.join(a.out, "splats.usda"), "w") as f:
    f.write(header + 'def Xform "World"\n{\n' + cam_block)
    f.write('    def ParticleField3DGaussianSplat "Splats"\n    {\n')
    f.write("        matrix4d xformOp:transform = %s\n" % usd_matrix(M))
    f.write('        uniform token[] xformOpOrder = ["xformOp:transform"]\n')
    f.write("        point3f[] positions = [%s]\n" % ", ".join(f3(p) for p in pos))
    f.write("        quatf[] orientations = [%s]\n" % ", ".join("(%r, %r, %r, %r)" % q for q in rot))
    f.write("        float3[] scales = [%s]\n" % ", ".join(f3(s) for s in scl))
    f.write("        float[] opacities = [%s]\n" % ", ".join(repr(b[3]) for b in base))
    f.write("        uniform int radiance:sphericalHarmonicsDegree = %d\n" % DEGREE)
    coeffs = []
    for i in range(n):
        coeffs.append(base[i][:3])
        coeffs += [rest[k][i] for k in range(NCOEF - 1)]
    f.write("        float3[] radiance:sphericalHarmonicsCoefficients = [%s]\n" %
            ", ".join(f3(c) for c in coeffs))
    f.write("    }\n}\n")

# Portable PLY in world space (PLY has no transform; Blender imports it with
# an identity object matrix). Positions, orientations and scales are baked
# with the object transform. SH is evaluated in the splat's object space, so
# the object rotation must also be applied to the SH bands: each band l >= 1
# is multiplied by its real-SH rotation matrix (fitted exactly by least
# squares, since a rotated band-l function stays in band l).


def sh_rotation_matrices(R):
    """Real-SH rotation matrices for bands 1..3 in the 3DGS/Cycles basis
    (m = -l..l), fitted on sample directions."""
    def basis(v):
        x, y, z = v
        return [
            [-0.488602512 * y, 0.488602512 * z, -0.488602512 * x],
            [1.092548431 * x * y, -1.092548431 * y * z,
             0.315391565 * (3 * z * z - 1), -1.092548431 * x * z,
             0.546274215 * (x * x - y * y)],
            [-0.590043589 * y * (3 * x * x - y * y), 2.890611442 * x * y * z,
             -0.457045799 * y * (5 * z * z - 1), 0.373176332 * z * (5 * z * z - 3),
             -0.457045799 * x * (5 * z * z - 1), 1.445305721 * z * (x * x - y * y),
             -0.590043590 * x * (x * x - 3 * y * y)]]
    srng = random.Random(1)
    dirs = []
    for _ in range(64):
        v = Vector((srng.gauss(0, 1), srng.gauss(0, 1), srng.gauss(0, 1))).normalized()
        dirs.append(v)
    mats = []
    for band in range(3):
        size = 2 * band + 3
        # f_world(d) = f_obj(R^-1 d)  =>  B(R^-1 d) = D * B(d)  solve D
        A = [basis(tuple(d))[band] for d in dirs]
        Bm = [basis(tuple(R.inverted() @ d))[band] for d in dirs]
        # Solve D^T = (A^T A)^-1 A^T Bm
        A_ = np.array(A); B_ = np.array(Bm)
        DT, *_ = np.linalg.lstsq(A_, B_, rcond=None)
        mats.append(DT.T)
    return mats


Rm = qobj.to_matrix()
D = sh_rotation_matrices(Rm)


def write_ply():
  with open(os.path.join(a.out, "splats.ply"), "wb") as f:
      props = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"]
      props += ["f_rest_%d" % i for i in range((NCOEF - 1) * 3)]
      props += ["opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"]
      f.write(("ply\nformat binary_little_endian 1.0\nelement vertex %d\n" % n +
               "".join("property float %s\n" % p for p in props) + "end_header\n").encode())
      for i in range(n):
          wp = M @ Vector(pos[i])
          wq = qobj @ Quaternion(rot[i])
          ws = [v * uniform for v in scl[i]]
          coef = np.array([rest[k][i] for k in range(NCOEF - 1)]).reshape(NCOEF - 1, 3)
          rotated = np.zeros_like(coef)
          start = 0
          for band in range(DEGREE):
              size = 2 * band + 3
              # coefficients c' such that sum c'_m B_m(d) = sum c_m B_m(R^-1 d)
              rotated[start:start + size] = D[band].T @ coef[start:start + size]
              start += size
          rest_cm = [rotated[k][ch] for ch in range(3) for k in range(NCOEF - 1)]
          o = min(max(base[i][3], 1e-6), 1 - 1e-6)
          vals = list(wp) + list(base[i][:3]) + rest_cm
          vals += [math.log(o / (1 - o))] + [math.log(v) for v in ws] + list(wq)
          f.write(struct.pack("<%df" % len(vals), *vals))

if UNIFORM:
    write_ply()
    with open(os.path.join(a.out, "scene_ply.usda"), "w") as f:
        f.write(header + 'def Xform "World"\n{\n' + cam_block)
        f.write('    def "Splats" (\n        references = @./splats.ply@\n    )\n    {\n    }\n}\n')

bpy.ops.wm.usd_export(filepath=os.path.join(a.out, "blender_native.usda"))

# ---- Cycles references ------------------------------------------------------
world = bpy.data.worlds.new("W")
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs[0].default_value = WORLD + (1.0,)
bg.inputs[1].default_value = 1.0 if any(WORLD) else 0.0
scene.world = world
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = a.spp
scene.cycles.use_denoising = False
scene.cycles.filter_width = 0.01
scene.render.resolution_x = a.width
scene.render.resolution_y = a.height
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.render.dither_intensity = 0.0  # no output noise; 8-bit rounds like ours
if a.exr:
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_depth = "32"
else:
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_depth = "8"
EXT = ".exr" if a.exr else ".png"


def render(path, seed):
    scene.cycles.seed = seed
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


render(os.path.join(a.out, "ref_seed0" + EXT), 0)
render(os.path.join(a.out, "ref_seed7" + EXT), 7)
# Portable round trip inside Blender: replace the native object with the PLY.
if UNIFORM:
    bpy.data.objects.remove(obj)
    bpy.ops.wm.ply_import(filepath=os.path.join(a.out, "splats.ply"))
    render(os.path.join(a.out, "ref_from_ply" + EXT), 0)
print("EXPORTED", n, "splats to", a.out)
