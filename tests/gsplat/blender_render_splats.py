# SPDX-License-Identifier: Apache-2.0
# Blender 5.3 script: import a 3DGS PLY and render it with Cycles using the
# camera written by make_cycles_fixture.py.
# blender -b --factory-startup --python blender_render_splats.py -- \
#     splats.ply out.png WIDTH HEIGHT SAMPLES
import sys

import bpy

ply, out, width, height, samples = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.ply_import(filepath=ply)
obj = bpy.context.selected_objects[0]
print("IMPORTED", obj.type, getattr(obj.data, "type", None), len(obj.data.points))

scene = bpy.context.scene
cam_data = bpy.data.cameras.new("Cam")
cam_data.lens = 50.0
cam_data.sensor_fit = "HORIZONTAL"
cam_data.sensor_width = 36.0
cam_data.clip_start = 0.01
cam_data.clip_end = 1000.0
cam = bpy.data.objects.new("Cam", cam_data)
cam.location = (0.0, 0.0, 3.0)  # looks down -Z, +Y up, like the USD camera
scene.collection.objects.link(cam)
scene.camera = cam

world = bpy.data.worlds.new("W")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[1].default_value = 0.0
scene.world = world

scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = int(samples)
scene.cycles.use_denoising = False
scene.cycles.filter_width = 0.01  # box-like; lusdrender uses jittered box
scene.cycles.seed = 0
scene.render.resolution_x = int(width)
scene.render.resolution_y = int(height)
scene.render.resolution_percentage = 100
scene.render.film_transparent = False
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.render.dither_intensity = 0.0  # no output noise; 8-bit rounds like ours
scene.view_settings.exposure = 0.0
scene.view_settings.gamma = 1.0
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_depth = "8"
scene.render.filepath = out
bpy.ops.render.render(write_still=True)
