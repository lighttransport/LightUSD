# SPDX-License-Identifier: Apache-2.0
"""Blender-runtime smoke test for the USD physics hook and Bullet solver.

Run with:
    blender -b --factory-startup --python blender/tests/test_usd_physics_runtime.py

This exercises Blender's real USD operators and bundled ``pxr`` modules. It is
not a cross-solver dynamics-equivalence test.
"""

import importlib.util
import math
import os
from pathlib import Path
import tempfile

import bpy
from pxr import Usd, UsdPhysics


ROOT = Path(__file__).resolve().parents[2]
HOOK_PATH = ROOT / 'blender' / 'usd_physics_hook.py'


def _assert_close(actual, expected, tolerance=1e-4):
    assert abs(actual - expected) <= tolerance, (actual, expected)


def _clear_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)


def _make_scene_and_export(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.gravity = (0.0, 0.0, -9.81)

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(1.0, 0.0, 0.0))
    body_a = bpy.context.object
    body_a.name = 'BodyA'
    bpy.ops.rigidbody.object_add(type='ACTIVE')
    body_a.rigid_body.mass = 2.5
    body_a.rigid_body.friction = 0.7
    body_a.rigid_body.restitution = 0.2
    body_a.rigid_body.collision_collections = [True] + [False] * 19

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(1.0, 2.0, 0.0))
    body_b = bpy.context.object
    body_b.name = 'BodyB'
    bpy.ops.rigidbody.object_add(type='PASSIVE')
    body_b.rigid_body.friction = 0.4
    body_b.rigid_body.collision_collections = [False, True] + [False] * 18

    bpy.ops.object.empty_add(location=(2.0, 0.0, 0.0))
    joint_obj = bpy.context.object
    joint_obj.name = 'SpringJoint'
    joint_obj.rotation_euler.z = math.pi / 2
    bpy.ops.rigidbody.constraint_add(type='GENERIC_SPRING')
    constraint = joint_obj.rigid_body_constraint
    constraint.object1 = body_a
    constraint.object2 = body_b
    constraint.use_spring_y = True
    constraint.spring_stiffness_y = 12.0
    constraint.spring_damping_y = 3.0
    constraint.use_spring_ang_z = True
    constraint.spring_stiffness_ang_z = 20.0
    constraint.spring_damping_ang_z = 4.0
    constraint.use_limit_lin_x = True
    constraint.limit_lin_x_lower = -0.4
    constraint.limit_lin_x_upper = 0.7
    constraint.use_limit_ang_z = True
    constraint.limit_ang_z_lower = -math.pi / 4
    constraint.limit_ang_z_upper = math.pi / 2

    result = bpy.ops.wm.usd_export(
        filepath=str(path), export_custom_properties=True,
        export_materials=False, export_animation=False,
    )
    assert 'FINISHED' in result, result

    stage = Usd.Stage.Open(str(path))
    assert stage, 'USD exporter output did not reopen'
    body_a_prim = next(p for p in stage.Traverse() if p.GetName() == 'BodyA')
    body_b_prim = next(p for p in stage.Traverse() if p.GetName() == 'BodyB')
    assert body_a_prim.HasAPI(UsdPhysics.RigidBodyAPI)
    assert body_b_prim.HasAPI(UsdPhysics.RigidBodyAPI)
    assert stage.GetPrimAtPath('/PhysicsScene')
    joint_prim = next(p for p in stage.Traverse()
                      if p.GetTypeName() == 'PhysicsJoint')
    assert joint_prim.HasAPI(UsdPhysics.DriveAPI, 'transY')
    assert joint_prim.HasAPI(UsdPhysics.DriveAPI, 'rotZ')
    assert stage.GetPrimAtPath('/BlenderPhysicsCollisionGroups')
    print('BLENDER_USD_EXPORT_PASS', body_a_prim.GetPath(),
          body_b_prim.GetPath(), joint_prim.GetPath())


def _check_import(path, merge_parent_xform):
    _clear_scene()
    result = bpy.ops.wm.usd_import(
        filepath=str(path), merge_parent_xform=merge_parent_xform,
    )
    assert 'FINISHED' in result, result
    objects = {obj.name: obj for obj in bpy.data.objects}
    body_a = objects.get('Cube')
    body_b = objects.get('Cube_001')
    assert body_a and body_a.rigid_body, 'BodyA collider did not regain rigid-body data'
    assert body_b and body_b.rigid_body, 'BodyB collider did not regain rigid-body data'
    _assert_close(body_a.rigid_body.mass, 2.5)
    assert body_a.rigid_body.collision_collections[0]
    assert not body_a.rigid_body.collision_collections[1]
    assert body_b.rigid_body.collision_collections[1]

    constraints = [obj for obj in bpy.data.objects if obj.rigid_body_constraint]
    assert constraints, 'spring joint was not restored'
    joint_obj = constraints[0]
    joint = joint_obj.rigid_body_constraint
    assert joint.type == 'GENERIC_SPRING'
    assert joint.object1 == body_a and joint.object2 == body_b
    assert joint.use_spring_y and joint.use_spring_ang_z
    _assert_close(joint.spring_stiffness_y, 12.0)
    _assert_close(joint.spring_stiffness_ang_z, 20.0, 1e-3)
    assert joint.use_limit_ang_z
    _assert_close(joint.limit_ang_z_lower, -math.pi / 4)
    _assert_close(joint_obj.matrix_world.translation.x, 2.0)
    _assert_close(joint_obj.matrix_world.translation.y, 0.0)
    joint_rotation = joint_obj.matrix_world.to_quaternion()
    _assert_close(abs(joint_rotation.w), math.sqrt(0.5), 1e-3)
    _assert_close(abs(joint_rotation.z), math.sqrt(0.5), 1e-3)
    assert bpy.context.scene.gravity.z < -9.8

    if not merge_parent_xform:
        assert objects['BodyA'].type == 'EMPTY'
        assert body_a.parent == objects['BodyA']
    print('BLENDER_USD_IMPORT_PASS',
          'merge_parent_xform=' + str(merge_parent_xform),
          body_a.name, body_b.name, joint.type)


def _check_bullet_dynamics():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.gravity = (0.0, 0.0, -9.81)
    scene.render.fps = 60
    scene.render.fps_base = 1.0
    scene.frame_start = 1
    scene.frame_end = 120

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, -0.5))
    floor = bpy.context.object
    floor.name = 'Floor'
    floor.dimensions = (20.0, 20.0, 1.0)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bpy.ops.rigidbody.object_add(type='PASSIVE')
    floor.rigid_body.collision_shape = 'BOX'
    floor.rigid_body.friction = 0.8

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 2.0))
    cube = bpy.context.object
    cube.name = 'DropCube'
    bpy.ops.rigidbody.object_add(type='ACTIVE')
    cube.rigid_body.collision_shape = 'BOX'
    cube.rigid_body.mass = 1.0
    cube.rigid_body.friction = 0.8
    cube.rigid_body.restitution = 0.0
    cube.rigid_body.linear_damping = 0.0
    cube.rigid_body.angular_damping = 0.0

    world = scene.rigidbody_world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 120

    def evaluated_z(obj):
        bpy.context.view_layer.update()
        return float(obj.evaluated_get(
            bpy.context.evaluated_depsgraph_get()
        ).matrix_world.translation.z)

    scene.frame_set(1)
    start_z = evaluated_z(cube)
    for frame in range(2, 31):
        scene.frame_set(frame)
    fall_z = evaluated_z(cube)
    for frame in range(31, 121):
        scene.frame_set(frame)
    settled_z = evaluated_z(cube)

    assert start_z > 1.9
    assert 0.5 < fall_z < start_z - 0.5, fall_z
    assert 0.45 <= settled_z <= 0.65, settled_z
    print('BLENDER_BULLET_DYNAMICS_PASS',
          {'z_frame1': start_z, 'z_frame30': fall_z,
           'z_frame120': settled_z, 'substeps': world.substeps_per_frame,
           'solver_iterations': world.solver_iterations})


def main():
    spec = importlib.util.spec_from_file_location('lightusd_usd_physics_hook', HOOK_PATH)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    assert hook._HAS_PXR and hasattr(bpy.types, 'USDHook')

    fd, path_string = tempfile.mkstemp(suffix='.usda', prefix='lightusd-physics-')
    os.close(fd)
    path = Path(path_string)
    path.unlink()
    try:
        hook.register()
        _make_scene_and_export(path)
        _check_import(path, merge_parent_xform=True)
        _check_import(path, merge_parent_xform=False)
        hook.unregister()
        _check_bullet_dynamics()
    finally:
        if path.exists():
            path.unlink()
        try:
            hook.unregister()
        except RuntimeError:
            pass

    print('BLENDER_USD_PHYSICS_RUNTIME_PASS', bpy.app.version_string)


if __name__ == '__main__':
    main()
