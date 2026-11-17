"""Generate the skinned test assets (run with Blender 5.x, headless):

  blender -b --factory-startup --python tests/obj-skel/make_assets.py -- OUTDIR

Writes into OUTDIR:
  rig.fbx  a 3-joint chain (root/spine/head) skinning a 4x4x12 column mesh,
           with a 24-frame action "bend" (Blender's FBX exporter)
  rig.obj         the same mesh in bind pose with `vw` skin weights; skel_joint
                  names/hierarchy, with bind/rest and the animation from rig.fbx
                  (skel_lib/skel_anim; tinyobjloader skel-obj.md)
  rig_inline.obj  the same, with a self-contained static skeleton (inline bind
                  and rest matrices, no sidecar)
  rig_blend.fbx   rig.fbx plus blend shapes: on the column "bulge" (animated
                  0 -> 1 -> 0) and "lean" (static 0.4), and an unskinned cube
                  "box" with an animated shape key "puff"

Coordinates: OBJ and FBX are Y-up. Blender is Z-up, so (x, y, z)_blender is
written as (x, z, -y). Matrices are row-major with the translation in the last
row (USD matrix4d).
"""
import math
import os
import sys

import bpy
from mathutils import Matrix

OUT = sys.argv[sys.argv.index("--") + 1]
os.makedirs(OUT, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 24
scene.frame_start, scene.frame_end = 1, 24

# --- mesh: a column 0.4 x 0.4 x 3, 12 segments high (13 rings of 4 vertices)
ring = [(-0.2, -0.2), (0.2, -0.2), (0.2, 0.2), (-0.2, 0.2)]
verts = [(x, y, 3.0 * k / 12) for k in range(13) for (x, y) in ring]
faces = []
for k in range(12):
    for i in range(4):
        a, b = 4 * k + i, 4 * k + (i + 1) % 4
        faces.append((a, b, b + 4, a + 4))
faces.append((3, 2, 1, 0))
faces.append(tuple(48 + i for i in range(4)))
me = bpy.data.meshes.new("body")
me.from_pydata(verts, [], faces)
me.validate()
mesh_ob = bpy.data.objects.new("body", me)
scene.collection.objects.link(mesh_ob)

# --- armature: root -> spine -> head along +Z (Blender)
bpy.ops.object.armature_add(enter_editmode=True, location=(0, 0, 0))
arm_ob = bpy.context.object
arm_ob.name = arm_ob.data.name = "rig"
eb = arm_ob.data.edit_bones
root = eb[0]
root.name = "root"
root.head, root.tail = (0, 0, 0), (0, 0, 1)
spine = eb.new("spine")
spine.head, spine.tail, spine.parent, spine.use_connect = (0, 0, 1), (0, 0, 2), root, True
head = eb.new("head")
head.head, head.tail, head.parent, head.use_connect = (0, 0, 2), (0, 0, 3), spine, True
bpy.ops.object.mode_set(mode='OBJECT')

mesh_ob.select_set(True)
arm_ob.select_set(True)
bpy.context.view_layer.objects.active = arm_ob
bpy.ops.object.parent_set(type='ARMATURE_AUTO')

# --- action "bend": spine and head rotate about X, 0 -> 40 -> 0 degrees
bpy.ops.object.mode_set(mode='POSE')
action = bpy.data.actions.new("bend")
arm_ob.animation_data_create().action = action
for frame, deg in ((1, 0), (12, 40), (24, 0)):
    for name in ("spine", "head"):
        pb = arm_ob.pose.bones[name]
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (math.radians(deg), 0, 0)
        pb.keyframe_insert("rotation_euler", frame=frame)
bpy.ops.object.mode_set(mode='OBJECT')
scene.frame_set(1)

bpy.ops.export_scene.fbx(filepath=os.path.join(OUT, "rig.fbx"), use_selection=False,
                         add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=False,
                         bake_anim_use_nla_strips=False, axis_forward='-Z', axis_up='Y',
                         apply_scale_options='FBX_SCALE_ALL', mesh_smooth_type='FACE')

# --- rig.obj / rig_inline.obj: bind-pose mesh, vw weights, skeleton lines
C = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))  # Z-up -> Y-up


def rows(t):
    """Blender (column-vector) matrix -> USD row-major text, last row = translation."""
    t = t.transposed()
    return " ".join(f"{t[r][c]:.6g}" for r in range(4) for c in range(4))


bones = [arm_ob.data.bones[n] for n in ("root", "spine", "head")]
index = {b.name: i for i, b in enumerate(bones)}
me = mesh_ob.data


def write(name, skeleton):
    lines = ["# skinned test asset (LightUSD tests/obj-skel); skeleton: tinyobjloader skel-obj.md"]
    lines += skeleton
    lines.append("o body")
    for v in me.vertices:
        p = mesh_ob.matrix_world @ v.co
        lines.append(f"v {p.x:.6g} {p.z:.6g} {-p.y:.6g}")
    groups = {g.index: g.name for g in mesh_ob.vertex_groups}
    for v in me.vertices:
        w = [(index[groups[g.group]], g.weight) for g in v.groups
             if groups[g.group] in index and g.weight > 0]
        if w:
            lines.append(f"vw {v.index} " + " ".join(f"{j} {x:.6g}" for j, x in w))
    for p in me.polygons:
        lines.append("f " + " ".join(str(i + 1) for i in p.vertices))
    with open(os.path.join(OUT, name), "w") as f:
        f.write("\n".join(lines) + "\n")


# rig.obj: names and hierarchy here; bind, rest and animation from rig.fbx
write("rig.obj", [f"skel_joint {b.name} {b.parent.name if b.parent else '-'}" for b in bones] +
      ["skel_lib rig.fbx", "skel_anim rig.fbx Scene"])  # the FBX take Blender writes for the active action
# rig_inline.obj: self-contained static skeleton. A change of world axes maps a
# joint's world transform W to C W; parent-relative transforms are unchanged.
inline = []
for b in bones:
    inline.append(f"skel_joint {b.name} {b.parent.name if b.parent else '-'} "
                  f"{rows(C @ arm_ob.matrix_world @ b.matrix_local)}")
for b in bones:
    # a root's rest transform is relative to the (Y-up) skeleton space
    local = C @ b.matrix_local if not b.parent else b.parent.matrix_local.inverted() @ b.matrix_local
    inline.append(f"skel_rest {b.name} {rows(local)}")
write("rig_inline.obj", inline)
# --- rig_blend.fbx: shape keys (FBX blend shape channels)
mesh_ob.shape_key_add(name="Basis")
bulge = mesh_ob.shape_key_add(name="bulge")
lean = mesh_ob.shape_key_add(name="lean")
for i, v in enumerate(mesh_ob.data.vertices):
    z = v.co.z
    f = max(0.0, 1.0 - abs(z - 1.5) / 1.0)  # around the middle
    bulge.data[i].co = (v.co.x * (1 + 0.8 * f), v.co.y * (1 + 0.8 * f), z)
    lean.data[i].co = (v.co.x + 0.3 * z / 3.0, v.co.y, z)
lean.value = 0.4
for frame, w in ((1, 0.0), (12, 1.0), (24, 0.0)):
    bulge.value = w
    bulge.keyframe_insert("value", frame=frame)
bpy.ops.mesh.primitive_cube_add(size=0.5, location=(1.5, 0, 0.25))
box = bpy.context.object
box.name = box.data.name = "box"
box.shape_key_add(name="Basis")
puff = box.shape_key_add(name="puff")
for i, v in enumerate(box.data.vertices):
    puff.data[i].co = v.co * 1.6 if v.co.z > 0 else v.co
for frame, w in ((1, 0.0), (12, 1.0), (24, 0.25)):
    puff.value = w
    puff.keyframe_insert("value", frame=frame)
scene.frame_set(1)
bpy.ops.export_scene.fbx(filepath=os.path.join(OUT, "rig_blend.fbx"), use_selection=False,
                         add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=False,
                         bake_anim_use_nla_strips=False, axis_forward='-Z', axis_up='Y',
                         apply_scale_options='FBX_SCALE_ALL', mesh_smooth_type='FACE')
print("wrote", sorted(os.listdir(OUT)))
