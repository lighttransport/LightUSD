"""UsdSkel + UsdMtlx + UsdPhysics example: a skinned, blend-shaped, animated
arm with an OpenPBR (MaterialX) material, plus a small rigid-body setup.

    python examples/build_features.py [out_dir]
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import miniusd  # noqa: E402
from build_scene import checker_png  # noqa: E402
from miniusd import geom, mtlx, physics, skel  # noqa: E402


def box_mesh(x0, x1, y0, y1, z0, z1, segments=4):
    """A box subdivided along +X (so it can bend)."""
    pts, counts, idx = [], [], []
    for i in range(segments + 1):
        x = x0 + (x1 - x0) * i / segments
        pts += [(x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1)]
    for i in range(segments):
        a, b = 4 * i, 4 * (i + 1)
        for k in range(4):
            k2 = (k + 1) % 4
            counts.append(4)
            idx += [a + k, b + k, b + k2, a + k2]
    counts += [4, 4]
    idx += [0, 3, 2, 1, 4 * segments, 4 * segments + 1, 4 * segments + 2, 4 * segments + 3]
    return pts, counts, idx


def build():
    stage = miniusd.Stage(up_axis="Y", meters_per_unit=1.0)
    stage.start_time_code, stage.end_time_code, stage.time_codes_per_second = 1, 24, 24
    geom.add_xform(stage, "/World")

    # --- MaterialX (OpenPBR, Blender-style with UsdPreviewSurface fallback)
    skin_mat = mtlx.add_openpbr_material(stage, "/World/Looks/Skin", base_color=(0.8, 0.5, 0.4),
                                         roughness=0.45, base_color_texture="textures/skin.png")
    metal = mtlx.add_openpbr_material(stage, "/World/Looks/Metal", base_color=(0.9, 0.9, 0.92),
                                      metalness=1.0, roughness=0.2)

    # --- UsdSkel: two-joint arm, skinned + a blend shape, animated
    root = skel.add_skel_root(stage, "/World/Arm")
    joints = ["Shoulder", "Shoulder/Elbow"]
    bind = [skel.translation_matrix((0, 0, 0)), skel.translation_matrix((2, 0, 0))]
    sk = skel.add_skeleton(stage, "/World/Arm/Skel", joints, bind_transforms=bind)
    pts, counts, idx = box_mesh(0, 4, -0.3, 0.3, -0.3, 0.3, segments=8)
    ji, jw = skel.skin_weights_from_nearest(pts, [(1, 0, 0), (3, 0, 0)], element_size=2)
    mesh = geom.add_mesh(stage, "/World/Arm/Mesh", pts, counts, idx)
    skel.bind_skin(mesh, sk, ji, jw, element_size=2)
    bulge = [(0, 0.15, 0) if p[1] > 0 else (0, -0.15, 0) for p in pts]
    skel.add_blendshape(mesh, "Bulge", bulge, point_indices=list(range(len(pts))))
    rest_rot = (1, 0, 0, 0)
    bend = skel.quat_from_axis_angle((0, 0, 1), 90)
    anim = skel.add_animation(
        stage, "/World/Arm/Skel/Anim", joints,
        translations=[(0, 0, 0), (2, 0, 0)],
        rotations={1: [rest_rot, rest_rot], 24: [rest_rot, bend]},
        scales=[(1, 1, 1), (1, 1, 1)],
        blend_shapes=["Bulge"], blend_shape_weights={1: [0.0], 24: [1.0]})
    skel.bind_animation(sk, anim)
    geom.bind_material(mesh, skin_mat)

    # --- UsdPhysics: ground + falling box + pendulum on a driven hinge
    physics.add_scene(stage, "/World/PhysicsScene", gravity_direction=(0, -1, 0), gravity_magnitude=9.81)
    ground = geom.add_cube(stage, "/World/Ground", size=1.0)
    geom.set_transform(ground, translate=(0, -0.5, 0), scale=(20, 1, 20))
    physics.add_collider(ground)
    ice = physics.add_material(stage, "/World/Looks/Ice", static_friction=0.05, dynamic_friction=0.03,
                               restitution=0.1)
    physics.bind_material(ground, ice)

    box = geom.add_cube(stage, "/World/Box", size=0.5)
    geom.set_transform(box, translate=(0, 3, 2))
    physics.add_rigid_body(box, mass=2.0, angular_velocity=(0, 90, 0))
    physics.add_collider(box)
    physics.set_material(box, static_friction=0.6, dynamic_friction=0.6, restitution=0.2)
    geom.bind_material(box, metal)

    anchor = geom.add_cube(stage, "/World/Anchor", size=0.2)
    geom.set_transform(anchor, translate=(-3, 3, 0))
    physics.add_rigid_body(anchor, kinematic=True)
    bob = geom.add_sphere(stage, "/World/Bob", radius=0.3)
    geom.set_transform(bob, translate=(-3, 1.5, 0))
    physics.add_rigid_body(bob, density=500.0)
    physics.add_collider(bob)
    hinge = physics.add_joint(stage, "/World/Hinge", "revolute", body0=anchor, body1=bob,
                              local_pos0=(0, 0, 0), local_pos1=(0, 1.5, 0), axis="Z",
                              lower_limit=-60, upper_limit=60)
    physics.add_drive(hinge, "angular", drive_type="force", stiffness=50, damping=5, target_position=30)
    physics.filter_pairs(anchor, bob)
    return stage


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    stage = build()
    png = checker_png()  # texture creation is the app's job; a tiny generated PNG here
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    with open(os.path.join(out, "textures", "skin.png"), "wb") as f:
        f.write(png)
    for ext in ("usda", "usdc"):
        print(stage.save(os.path.join(out, "features." + ext)))
    print(stage.save(os.path.join(out, "features.usdz"), assets={"textures/skin.png": png}))
    with open(os.path.join(out, "Skin.mtlx"), "w") as f:
        f.write(mtlx.material_to_mtlx(stage, "/World/Looks/Skin"))
    print(os.path.join(out, "Skin.mtlx"))
