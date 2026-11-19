# SPDX-License-Identifier: Apache-2.0
"""Blender 5.3: create Gaussian splats natively, export them, render Cycles refs.

blender -b --factory-startup --python blender_make_and_export.py -- \
    OUT_DIR [--count N] [--seed S] [--width W] [--height H] [--spp N]
    [--degree 0-3] [--scale X,Y,Z] [--ortho] [--world R,G,B] [--motion]
    [--camera-motion] [--colour-motion] [--exr]

Variants: --degree sets the SH degree; --scale gives a non-uniform object
scale (not representable in a transform-free PLY, so the PLY outputs and
ref_from_ply are skipped); --ortho uses an orthographic camera; --world sets
a constant world background colour (strength 1); --motion animates the
splat object (location, rotation, scale; linear keys around frame 1) and
renders frame 1 with Cycles motion blur (shutter 0.5 frames, centred). The
USDA then carries xformOp:transform time samples over the shutter and the
camera's shutter:open/close (PLY outputs are skipped). --camera-motion
animates the camera (exported as camera transform time samples);
--colour-motion animates every splat's radiance:base (DC colour and opacity)
through a Geometry Nodes modifier, exported from Blender's evaluated data as
SH coefficient and opacity time samples. Any motion flag enables blur.

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
ap.add_argument("--motion", action="store_true")
ap.add_argument("--camera-motion", action="store_true")
ap.add_argument("--colour-motion", action="store_true")
ap.add_argument("--colour-only", action="store_true",
                help="with --colour-motion: animate colour only (no position "
                     "offset): Cycles then uses the frame-time colour")
ap.add_argument("--exr", action="store_true",
                help="write references as 32-bit scene-linear EXR (no view transform)")
a = ap.parse_args(argv)
os.makedirs(a.out, exist_ok=True)
DEGREE = a.degree
OBJ_SCALE = tuple(float(v) for v in a.scale.split(","))
WORLD = tuple(float(v) for v in a.world.split(","))
BLUR = a.motion or a.camera_motion or a.colour_motion
UNIFORM = max(OBJ_SCALE) - min(OBJ_SCALE) < 1e-9 and not BLUR
SHUTTER = 0.5  # frames, centred on the frame
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

def linear_keys(o):
    act = o.animation_data.action
    curves = list(getattr(act, "fcurves", []) or [])
    for layer in getattr(act, "layers", []):  # Blender 5.x layered actions
        for strip in layer.strips:
            for bag in strip.channelbags:
                curves += list(bag.fcurves)
    for fc in curves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"


obj = bpy.data.objects.new("Splats", pc)
scene.collection.objects.link(obj)
# Rotation + uniform scale + translation: exactly representable after baking
# into the portable (transform-free) PLY.
obj.location = (0.1, -0.05, 0.2)
obj.rotation_euler = (0.4, -0.3, 0.7)
obj.scale = OBJ_SCALE
if a.motion:
    base_loc = obj.location.copy()
    base_rot = obj.rotation_euler.copy()
    base_scl = obj.scale.copy()
    for frame, sgn in ((0, -1.0), (1, 0.0), (2, 1.0)):
        obj.location = base_loc + Vector((0.30, 0.12, -0.05)) * sgn
        obj.rotation_euler = (base_rot[0], base_rot[1] + 0.15 * sgn,
                              base_rot[2] + 0.6 * sgn)
        obj.scale = base_scl * (1.0 + 0.2 * sgn)
        obj.keyframe_insert("location", frame=frame)
        obj.keyframe_insert("rotation_euler", frame=frame)
        obj.keyframe_insert("scale", frame=frame)
    act = obj.animation_data.action
    curves = list(getattr(act, "fcurves", []) or [])
    for layer in getattr(act, "layers", []):  # Blender 5.x layered actions
        for strip in layer.strips:
            for bag in strip.channelbags:
                curves += list(bag.fcurves)
    for fc in curves:
        for kp in fc.keyframe_points:
            kp.interpolation = "LINEAR"
    scene.frame_set(1)

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
if a.camera_motion:
    base_cl, base_cr = cam.location.copy(), cam.rotation_euler.copy()
    for frame, sgn in ((0, -1.0), (1, 0.0), (2, 1.0)):
        cam.location = base_cl + Vector((0.35, 0.15, 0.10)) * sgn
        cam.rotation_euler = (base_cr[0] + 0.05 * sgn, base_cr[1], base_cr[2] + 0.12 * sgn)
        cam.keyframe_insert("location", frame=frame)
        cam.keyframe_insert("rotation_euler", frame=frame)
    linear_keys(cam)
if a.colour_motion:
    # radiance:base <- mix(radiance:base, target, f(frame)), f keyed 0 -> 1
    # over frames 0..2 (DC colour and opacity both animate). Positions are
    # offset by f too: Cycles interpolates radiance:base only for deforming
    # (motion) Gaussian splat primitives.
    ng = bpy.data.node_groups.new("ColourAnim", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    gi = ng.nodes.new("NodeGroupInput"); go = ng.nodes.new("NodeGroupOutput")
    na = ng.nodes.new("GeometryNodeInputNamedAttribute")
    na.data_type = "FLOAT_COLOR"; na.inputs["Name"].default_value = "radiance:base"
    mix = ng.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"; mix.blend_type = "MIX"
    st = ng.nodes.new("GeometryNodeStoreNamedAttribute")
    st.data_type = "FLOAT_COLOR"; st.domain = "POINT"
    st.inputs["Name"].default_value = "radiance:base"
    sp = ng.nodes.new("GeometryNodeSetPosition")
    vm = ng.nodes.new("ShaderNodeVectorMath"); vm.operation = "SCALE"
    vm.inputs[0].default_value = (0.0, 0.0, 0.0) if a.colour_only else (0.6, 0.25, 0.1)
    fval = ng.nodes.new("ShaderNodeValue")
    ng.links.new(fval.outputs[0], vm.inputs["Scale"])
    ng.links.new(vm.outputs[0], sp.inputs["Offset"])
    ng.links.new(gi.outputs[0], sp.inputs["Geometry"])
    ng.links.new(sp.outputs[0], st.inputs["Geometry"])
    ng.links.new(st.outputs[0], go.inputs[0])
    col_a = [s for s in mix.inputs if s.name == "A" and s.type == "RGBA"][0]
    col_b = [s for s in mix.inputs if s.name == "B" and s.type == "RGBA"][0]
    col_out = [s for s in mix.outputs if s.name == "Result" and s.type == "RGBA"][0]
    ng.links.new([s for s in na.outputs if s.type == "RGBA"][0], col_a)
    col_b.default_value = (1.4, -1.2, 0.6, 0.55)
    ng.links.new(col_out, [s for s in st.inputs if s.name == "Value"][0])
    ng.links.new(fval.outputs[0], [s for s in mix.inputs if s.name == "Factor"][0])
    fac = fval.outputs[0]
    # Full swing inside the shutter (frames 0.75 .. 1.25) so the trail's
    # colour gradient is clearly visible.
    for frame, v in ((1.0 - SHUTTER / 2, 0.0), (1.0 + SHUTTER / 2, 1.0)):
        fac.default_value = v
        fac.keyframe_insert("default_value", frame=frame)
    linear_keys(ng)
    mod = obj.modifiers.new("ColourAnim", "NODES"); mod.node_group = ng
scene.frame_set(1)
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
if a.camera_motion:
    csamples = []
    for j in range(17):
        sub = -SHUTTER / 2 + SHUTTER * j / 16.0
        fr = int(math.floor(1 + sub))
        scene.frame_set(fr, subframe=(1 + sub) - fr)
        csamples.append("%r: %s" % (1 + sub, usd_matrix(cam.matrix_world)))
    scene.frame_set(1)
    cam_block = cam_block.replace(
        "        matrix4d xformOp:transform = %s\n" % usd_matrix(cam.matrix_world),
        "        matrix4d xformOp:transform.timeSamples = {\n            %s\n        }\n"
        % ",\n            ".join(csamples), 1)
if BLUR:
    cam_block = cam_block.replace(
        "        float2 clippingRange",
        "        double shutter:open = %r\n        double shutter:close = %r\n"
        "        float2 clippingRange" % (-SHUTTER / 2, SHUTTER / 2), 1)
header = ('#usda 1.0\n(\n    defaultPrim = "World"\n    metersPerUnit = 1\n'
          + ('    startTimeCode = 0\n    endTimeCode = 2\n' if BLUR else '')
          + '    upAxis = "Z"\n)\n\n')

with open(os.path.join(a.out, "splats.usda"), "w") as f:
    f.write(header + 'def Xform "World"\n{\n' + cam_block)
    f.write('    def ParticleField3DGaussianSplat "Splats"\n    {\n')
    if a.motion:
        samples = []
        for j in range(17):
            sub = -SHUTTER / 2 + SHUTTER * j / 16.0
            fr = int(math.floor(1 + sub))
            scene.frame_set(fr, subframe=(1 + sub) - fr)
            samples.append("%r: %s" % (1 + sub, usd_matrix(obj.matrix_world)))
        scene.frame_set(1)
        f.write("        matrix4d xformOp:transform.timeSamples = {\n            %s\n        }\n"
                % ",\n            ".join(samples))
    else:
        f.write("        matrix4d xformOp:transform = %s\n" % usd_matrix(M))
    f.write('        uniform token[] xformOpOrder = ["xformOp:transform"]\n')
    if not a.colour_motion:
        f.write("        point3f[] positions = [%s]\n" % ", ".join(f3(p) for p in pos))
    f.write("        quatf[] orientations = [%s]\n" % ", ".join("(%r, %r, %r, %r)" % q for q in rot))
    f.write("        float3[] scales = [%s]\n" % ", ".join(f3(s) for s in scl))
    f.write("        uniform int radiance:sphericalHarmonicsDegree = %d\n" % DEGREE)

    def coeff_text(bases):
        coeffs = []
        for i in range(n):
            coeffs.append(bases[i][:3])
            coeffs += [rest[k][i] for k in range(NCOEF - 1)]
        return "[%s]" % ", ".join(f3(c) for c in coeffs)
    if a.colour_motion:
        # Blender's evaluated radiance:base at each subframe (exact).
        osamp, csamp, psamp = [], [], []
        for j in range(17):
            sub = -SHUTTER / 2 + SHUTTER * j / 16.0
            fr = int(math.floor(1 + sub))
            scene.frame_set(fr, subframe=(1 + sub) - fr)
            dg = bpy.context.evaluated_depsgraph_get()
            attr = obj.evaluated_get(dg).data.attributes["radiance:base"]
            flat = [0.0] * (4 * n)
            for key in ("color", "value", "vector"):
                try:
                    attr.data.foreach_get(key, flat)
                    break
                except (TypeError, AttributeError):
                    continue
            ev = [tuple(flat[4 * i:4 * i + 4]) for i in range(n)]
            pflat = [0.0] * (3 * n)
            obj.evaluated_get(dg).data.attributes["position"].data.foreach_get("vector", pflat)
            psamp.append("%r: [%s]" % (1 + sub, ", ".join(
                f3(pflat[3 * i:3 * i + 3]) for i in range(n))))
            osamp.append("%r: [%s]" % (1 + sub, ", ".join(repr(b[3]) for b in ev)))
            csamp.append("%r: %s" % (1 + sub, coeff_text(ev)))
        scene.frame_set(1)
        f.write("        float[] opacities.timeSamples = {\n            %s\n        }\n"
                % ",\n            ".join(osamp))
        f.write("        point3f[] positions.timeSamples = {\n            %s\n        }\n"
                % ",\n            ".join(psamp))
        f.write("        float3[] radiance:sphericalHarmonicsCoefficients.timeSamples = {\n"
                "            %s\n        }\n" % ",\n            ".join(csamp))
    else:
        f.write("        float[] opacities = [%s]\n" % ", ".join(repr(b[3]) for b in base))
        f.write("        float3[] radiance:sphericalHarmonicsCoefficients = %s\n" %
                coeff_text(base))
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

if not BLUR:
    bpy.ops.wm.usd_export(filepath=os.path.join(a.out, "blender_native.usda"))

# ---- Cycles references ------------------------------------------------------
world = bpy.data.worlds.new("W")
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs[0].default_value = WORLD + (1.0,)
bg.inputs[1].default_value = 1.0 if any(WORLD) else 0.0
scene.world = world
scene.render.engine = "CYCLES"
if BLUR:
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = SHUTTER
    scene.render.motion_blur_position = "CENTER"
    obj.cycles.motion_steps = 4   # 2^4 + 1 transform samples over the shutter
    obj.cycles.use_deform_motion = True  # attribute (colour) motion steps
    cam.cycles.motion_steps = 4
    scene.frame_set(1)
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
