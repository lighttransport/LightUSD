# SPDX-License-Identifier: Apache-2.0
"""Blender 5.3: mixed mesh + Gaussian splat scene, exported to USDA, with
direct-light-only Cycles references.

blender -b --factory-startup --python blender_mixed_scene.py -- \
    OUT_DIR [--width W] [--height H] [--spp N] [--no-splats] [--black]
    [--motion]

Scene: a Lambert ground plane (Diffuse BSDF, albedo 0.8) lit by a sun lamp,
plus a cluster of emissive splats floating above it. Cycles light paths are
limited to direct lighting (max_bounces = 0), which is what lusdrender's
direct-light integrator computes, so the comparison isolates splat
visibility: splats seen by the camera (emission), the plane seen through
splat transparency, and splats stochastically blocking sun shadow rays.

Writes mixed.usda (UsdPreviewSurface plane, DistantLight, camera, splats) and
ref_seed0.exr / ref_seed7.exr (scene-linear).
"""
import argparse
import math
import os
import random
import sys

import bpy
from mathutils import Quaternion, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--width", type=int, default=320)
ap.add_argument("--height", type=int, default=180)
ap.add_argument("--spp", type=int, default=256)
ap.add_argument("--no-splats", action="store_true")
ap.add_argument("--black", action="store_true",
                help="splats emit nothing (SH DC = -0.5/C0): isolates their "
                     "occlusion of the plane and of the sun")
ap.add_argument("--motion", action="store_true",
                help="animate the splat object; render frame 1 with Cycles "
                     "motion blur (shutter 0.5, centred)")
a = ap.parse_args(argv)
SHUTTER = 0.5
os.makedirs(a.out, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
rng = random.Random(11)

ALBEDO = 0.8
SUN_STRENGTH = 3.0                      # W/m^2 (Blender irradiance units)
SUN_DIR = Vector((0.35, -0.25, -1.0)).normalized()  # direction light travels

# ---- ground plane ----------------------------------------------------------
HALF = 2.0
verts = [(-HALF, -HALF, 0), (HALF, -HALF, 0), (HALF, HALF, 0), (-HALF, HALF, 0)]
me = bpy.data.meshes.new("Ground")
me.from_pydata(verts, [], [(0, 1, 2, 3)])
ground = bpy.data.objects.new("Ground", me)
scene.collection.objects.link(ground)
mat = bpy.data.materials.new("Lambert")
mat.use_nodes = True
nt = mat.node_tree
nt.nodes.clear()
dif = nt.nodes.new("ShaderNodeBsdfDiffuse")
dif.inputs["Color"].default_value = (ALBEDO, ALBEDO, ALBEDO, 1)
dif.inputs["Roughness"].default_value = 0.0
outn = nt.nodes.new("ShaderNodeOutputMaterial")
nt.links.new(dif.outputs[0], outn.inputs["Surface"])
me.materials.append(mat)

# ---- sun -------------------------------------------------------------------
sun_data = bpy.data.lights.new("Sun", "SUN")
sun_data.energy = SUN_STRENGTH
sun_data.angle = 0.0
sun = bpy.data.objects.new("Sun", sun_data)
sun.rotation_euler = SUN_DIR.to_track_quat("-Z", "Y").to_euler()
scene.collection.objects.link(sun)

# ---- splats ----------------------------------------------------------------
n = 0 if a.no_splats else 160
pos, base, scl, rot = [], [], [], []
for i in range(n):
    p = (rng.uniform(-0.6, 0.6), rng.uniform(-0.6, 0.6), rng.uniform(0.5, 1.0))
    q = Quaternion([rng.gauss(0, 1) for _ in range(4)]).normalized()
    s = (rng.uniform(0.05, 0.16), rng.uniform(0.05, 0.16), rng.uniform(0.02, 0.08))
    o = rng.uniform(0.4, 0.95)
    dc = (rng.uniform(-1.2, 1.2), rng.uniform(-1.2, 1.2), rng.uniform(-1.2, 1.2))
    if a.black:
        dc = (-0.5 / 0.28209479177387814,) * 3
    pos.append(p); rot.append(tuple(q)); scl.append(s); base.append(dc + (o,))
if n:
    pc = bpy.data.pointclouds.new("Splats")
    pc.resize(n)
    pc.type = "GAUSSIAN_SPLAT"

    def put(name, dtype, values):
        attr = pc.attributes.get(name) or pc.attributes.new(name, dtype, "POINT")
        flat = [c for v in values for c in v]
        key = "value" if dtype in ("FLOAT4", "QUATERNION") else "vector"
        try:
            attr.data.foreach_set(key, flat)
        except (TypeError, AttributeError):
            attr.data.foreach_set("vector" if key == "value" else "value", flat)

    put("position", "FLOAT_VECTOR", pos)
    put("radiance:base", "FLOAT4", base)
    put("scale", "FLOAT_VECTOR", scl)
    put("rotation", "QUATERNION", rot)
    splats = bpy.data.objects.new("Splats", pc)
    scene.collection.objects.link(splats)
    if a.motion:
        for frame, sgn in ((0, -1.0), (1, 0.0), (2, 1.0)):
            splats.location = Vector((0.40, 0.25, 0.0)) * sgn
            splats.rotation_euler = (0.0, 0.0, 0.5 * sgn)
            splats.keyframe_insert("location", frame=frame)
            splats.keyframe_insert("rotation_euler", frame=frame)
        act = splats.animation_data.action
        curves = list(getattr(act, "fcurves", []) or [])
        for layer in getattr(act, "layers", []):
            for strip in layer.strips:
                for bag in strip.channelbags:
                    curves += list(bag.fcurves)
        for fc in curves:
            for kp in fc.keyframe_points:
                kp.interpolation = "LINEAR"
        scene.frame_set(1)

# ---- camera ----------------------------------------------------------------
cam_data = bpy.data.cameras.new("Cam")
cam_data.lens = 35.0
cam_data.sensor_fit = "HORIZONTAL"
cam_data.sensor_width = 36.0
cam_data.clip_start = 0.01
cam_data.clip_end = 1000.0
cam = bpy.data.objects.new("Cam", cam_data)
cam.location = (2.6, -3.4, 2.4)
scene.collection.objects.link(cam)
cam.rotation_euler = (Vector((0, 0, 0.35)) - cam.location).to_track_quat("-Z", "Y").to_euler()
scene.camera = cam
bpy.context.view_layer.update()


# ---- USDA export -----------------------------------------------------------
def usd_matrix(m):  # Blender column-vector -> USD row-vector (transpose)
    return "( " + ", ".join(
        "(%r, %r, %r, %r)" % tuple(m[r][c] for r in range(4)) for c in range(4)) + " )"


f3 = lambda v: "(%r, %r, %r)" % tuple(v)
with open(os.path.join(a.out, "mixed.usda"), "w") as f:
    f.write('#usda 1.0\n(\n    defaultPrim = "World"\n    metersPerUnit = 1\n'
            + ('    startTimeCode = 0\n    endTimeCode = 2\n' if a.motion else '')
            + '    upAxis = "Z"\n)\n\ndef Xform "World"\n{\n')
    cam_block = ('    def Camera "Cam"\n    {\n'
                 "        float focalLength = %r\n        float horizontalAperture = %r\n"
                 "        float verticalAperture = %r\n        float2 clippingRange = (0.01, 1000)\n"
                 "        matrix4d xformOp:transform = %s\n"
                 '        uniform token[] xformOpOrder = ["xformOp:transform"]\n    }\n' %
                 (cam_data.lens, cam_data.sensor_width,
                  cam_data.sensor_width * a.height / a.width,
                  usd_matrix(cam.matrix_world)))
    if a.motion:
        cam_block = cam_block.replace(
            "        float2 clippingRange",
            "        double shutter:open = %r\n        double shutter:close = %r\n"
            "        float2 clippingRange" % (-SHUTTER / 2, SHUTTER / 2), 1)
    f.write(cam_block)
    # UsdLux DistantLight shines along its local -Z; intensity is irradiance
    # (W/m^2 in Blender's sun units maps 1:1 with exposure 0).
    f.write('    def DistantLight "Sun"\n    {\n'
            "        float inputs:intensity = %r\n        float inputs:angle = 0\n"
            "        matrix4d xformOp:transform = %s\n"
            '        uniform token[] xformOpOrder = ["xformOp:transform"]\n    }\n' %
            (SUN_STRENGTH, usd_matrix(sun.matrix_world)))
    f.write('    def Mesh "Ground" (\n        prepend apiSchemas = ["MaterialBindingAPI"]\n    )\n'
            "    {\n        int[] faceVertexCounts = [4]\n"
            "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
            "        point3f[] points = [%s]\n"
            "        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (\n"
            '            interpolation = "vertex"\n        )\n'
            '        uniform token subdivisionScheme = "none"\n'
            "        rel material:binding = </World/Lambert>\n    }\n" %
            ", ".join(f3(v) for v in verts))
    f.write('    def Material "Lambert"\n    {\n'
            "        token outputs:surface.connect = </World/Lambert/S.outputs:surface>\n"
            '        def Shader "S"\n        {\n'
            '            uniform token info:id = "UsdPreviewSurface"\n'
            "            color3f inputs:diffuseColor = (%r, %r, %r)\n"
            "            float inputs:roughness = 1\n"
            "            int inputs:useSpecularWorkflow = 1\n"
            "            color3f inputs:specularColor = (0, 0, 0)\n"
            "            token outputs:surface\n        }\n    }\n" % (ALBEDO, ALBEDO, ALBEDO))
    if n:
        f.write('    def ParticleField3DGaussianSplat "Splats"\n    {\n')
        if a.motion:
            samples = []
            for j in range(17):
                sub = -SHUTTER / 2 + SHUTTER * j / 16.0
                fr = int(math.floor(1 + sub))
                scene.frame_set(fr, subframe=(1 + sub) - fr)
                samples.append("%r: %s" % (1 + sub, usd_matrix(splats.matrix_world)))
            scene.frame_set(1)
            f.write("        matrix4d xformOp:transform.timeSamples = {\n            %s\n"
                    "        }\n        uniform token[] xformOpOrder = "
                    '["xformOp:transform"]\n' % ",\n            ".join(samples))
        f.write("        point3f[] positions = [%s]\n" % ", ".join(f3(p) for p in pos))
        f.write("        quatf[] orientations = [%s]\n" %
                ", ".join("(%r, %r, %r, %r)" % q for q in rot))
        f.write("        float3[] scales = [%s]\n" % ", ".join(f3(s) for s in scl))
        f.write("        float[] opacities = [%s]\n" % ", ".join(repr(b[3]) for b in base))
        f.write("        uniform int radiance:sphericalHarmonicsDegree = 0\n")
        f.write("        float3[] radiance:sphericalHarmonicsCoefficients = [%s]\n" %
                ", ".join(f3(b[:3]) for b in base))
        f.write("    }\n")
    f.write("}\n")

# ---- Cycles references (direct light only) ---------------------------------
world = bpy.data.worlds.new("W")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[1].default_value = 0.0
scene.world = world
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = a.spp
scene.cycles.use_denoising = False
scene.cycles.filter_width = 0.01
scene.cycles.max_bounces = 0
if a.motion:
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = SHUTTER
    scene.render.motion_blur_position = "CENTER"
    if n:
        splats.cycles.motion_steps = 4
    scene.frame_set(1)
scene.cycles.transparent_max_bounces = 64
scene.render.resolution_x = a.width
scene.render.resolution_y = a.height
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.render.image_settings.file_format = "OPEN_EXR"
scene.render.image_settings.color_depth = "32"
for seed in (0, 7):
    scene.cycles.seed = seed
    scene.render.filepath = os.path.join(a.out, "ref_seed%d.exr" % seed)
    bpy.ops.render.render(write_still=True)
print("EXPORTED mixed scene with", n, "splats to", a.out)
