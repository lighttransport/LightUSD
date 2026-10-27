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
from pxr import Gf, Sdf, Usd, UsdGeom, UsdPhysics


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
    body_a.scale = (2.0, 0.5, 1.5)
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
    for actual, expected in zip(body_a.matrix_world.to_scale(), (2.0, 0.5, 1.5)):
        _assert_close(actual, expected)
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
        assert body_a.parent is None
    print('BLENDER_USD_IMPORT_PASS',
          'merge_parent_xform=' + str(merge_parent_xform),
          body_a.name, body_b.name, joint.type)


def _make_collision_group_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    for index, name in enumerate(('A', 'B', 'C', 'D', 'E', 'Static')):
        cube = UsdGeom.Cube.Define(stage, f'/World/{name}')
        cube.CreateSizeAttr(1.0)
        prim = cube.GetPrim()
        UsdGeom.Xformable(prim).AddTranslateOp().Set(Gf.Vec3d(index * 2.0, 0.0, 0.0))
        UsdPhysics.CollisionAPI.Apply(prim)
        if name != 'Static':
            UsdPhysics.RigidBodyAPI.Apply(prim)

    def group(name, body_names, filtered=(), invert=False):
        result = UsdPhysics.CollisionGroup.Define(stage, f'/World/Groups/{name}')
        collection = result.GetCollidersCollectionAPI()
        collection.CreateExpansionRuleAttr().Set('explicitOnly')
        collection.CreateIncludesRel().SetTargets(
            [Sdf.Path(f'/World/{body}') for body in body_names])
        if filtered:
            result.CreateFilteredGroupsRel().SetTargets(
                [Sdf.Path(f'/World/Groups/{target}') for target in filtered])
        if invert:
            result.CreateInvertFilteredGroupsAttr().Set(True)
        return result

    group('GroupA', ('A',), filtered=('GroupB',))
    group('GroupB', ('B',))
    group('GroupD', ('D',), filtered=('GroupE',), invert=True)
    group('GroupE', ('E',))
    stage.GetRootLayer().Save()


def _check_collision_group_import(path):
    _clear_scene()
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    bodies = {name: bpy.data.objects.get(name)
              for name in ('A', 'B', 'C', 'D', 'E', 'Static')}
    assert all(obj and obj.rigid_body for obj in bodies.values()), bodies
    assert bodies['Static'].rigid_body.type == 'PASSIVE'

    def collides(first, second):
        a = bodies[first].rigid_body.collision_collections
        b = bodies[second].rigid_body.collision_collections
        return any(left and right for left, right in zip(a, b))

    assert not collides('A', 'B')
    assert collides('A', 'C') and collides('B', 'C')
    assert collides('D', 'E')
    assert not collides('D', 'C') and not collides('D', 'A')
    assert collides('E', 'C') and collides('E', 'A')
    assert not collides('D', 'Static') and collides('E', 'Static')
    print('BLENDER_USD_COLLISION_GROUP_IMPORT_PASS',
          {name: sum(1 << i for i, enabled in enumerate(
              obj.rigid_body.collision_collections) if enabled)
           for name, obj in bodies.items()})


def _make_mjcf_friction_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    scene = UsdPhysics.Scene.Define(stage, '/World/PhysicsScene')
    scene.CreateGravityDirectionAttr().Set(Gf.Vec3f(0.0, 0.0, -1.0))
    scene.CreateGravityMagnitudeAttr().Set(9.81)
    scene.GetPrim().CreateAttribute(
        'mjc:option:timestep', Sdf.ValueTypeNames.Double).Set(1.0 / 48.0)
    scene.GetPrim().CreateAttribute(
        'mjc:option:iterations', Sdf.ValueTypeNames.Int).Set(50)

    incline = UsdGeom.Cube.Define(stage, '/World/Incline')
    incline.CreateSizeAttr(2.0)
    incline_xform = UsdGeom.Xformable(incline.GetPrim())
    incline_xform.AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, -0.1))
    incline_xform.AddRotateYOp().Set(10.0)
    incline_xform.AddScaleOp().Set(Gf.Vec3f(5.0, 5.0, 0.1))
    UsdPhysics.CollisionAPI.Apply(incline.GetPrim())
    incline.GetPrim().CreateAttribute(
        'mjc:geomFriction', Sdf.ValueTypeNames.DoubleArray).Set(
            [0.8, 0.005, 0.0001])

    body = UsdGeom.Xform.Define(stage, '/World/Slider').GetPrim()
    UsdGeom.Xformable(body).AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 0.3))
    body_api = UsdPhysics.RigidBodyAPI.Apply(body)
    body_api.CreateRigidBodyEnabledAttr(True)
    UsdPhysics.MassAPI.Apply(body).CreateMassAttr(1.0)
    collider = UsdGeom.Cube.Define(stage, '/World/Slider/Geom')
    collider.CreateSizeAttr(0.2)
    UsdPhysics.CollisionAPI.Apply(collider.GetPrim())
    collider.GetPrim().CreateAttribute(
        'mjc:geomFriction', Sdf.ValueTypeNames.DoubleArray).Set(
            [0.8, 0.005, 0.0001])
    stage.GetRootLayer().Save()


def _check_mjcf_friction_dynamics(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 48
    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 73
    bodies = [obj for obj in bpy.data.objects if obj.rigid_body]
    slider = next(obj for obj in bodies if obj.rigid_body.type == 'ACTIVE')
    incline = next(obj for obj in bodies if obj.rigid_body.type == 'PASSIVE')
    _assert_close(slider.rigid_body.friction, 0.8)
    _assert_close(incline.rigid_body.friction, 0.8)

    samples = {}
    for frame in range(1, 74):
        scene.frame_set(frame)
        if frame in (1, 13, 25, 37, 49, 61, 73):
            bpy.context.view_layer.update()
            position = slider.evaluated_get(
                bpy.context.evaluated_depsgraph_get()).matrix_world.translation
            samples[frame] = tuple(float(position[i]) for i in range(3))
    assert samples[73][0] > 0.2, samples
    assert all(math.isfinite(value) for sample in samples.values() for value in sample)
    print('BLENDER_MUJOCO_FRICTION_DYNAMICS_PASS', samples,
          {'body_friction': slider.rigid_body.friction,
           'incline_friction': incline.rigid_body.friction})


def _make_multi_mesh_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    body = UsdGeom.Xform.Define(stage, '/World/MultiMeshBody').GetPrim()
    UsdPhysics.RigidBodyAPI.Apply(body)
    UsdPhysics.CollisionAPI.Apply(body)
    for index, name in enumerate(('PartA', 'PartB')):
        cube = UsdGeom.Cube.Define(stage, f'/World/MultiMeshBody/{name}')
        UsdGeom.Xformable(cube.GetPrim()).AddTranslateOp().Set(
            Gf.Vec3d(index * 1.5, 0.0, 0.0))
        UsdPhysics.CollisionAPI.Apply(cube.GetPrim())
    stage.GetRootLayer().Save()


def _check_multi_mesh_import(path):
    _clear_scene()
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=False)
    assert 'FINISHED' in result, result
    body = bpy.data.objects.get('MultiMeshBody')
    part_a = bpy.data.objects.get('PartA')
    part_b = bpy.data.objects.get('PartB')
    assert body and body.type == 'EMPTY'
    assert part_a and part_a.type == 'MESH'
    assert part_b and part_b.type == 'MESH'
    # The hook must not turn one USD rigid body into two independent Blender
    # bodies; it reports this ambiguous compound shape and leaves it untouched.
    assert not part_a.rigid_body and not part_b.rigid_body
    print('BLENDER_USD_MULTI_MESH_GUARD_PASS', body.name, part_a.name, part_b.name)


def _make_default_mass_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    scene = UsdPhysics.Scene.Define(stage, '/World/PhysicsScene')
    scene.CreateGravityDirectionAttr().Set(Gf.Vec3f(0.0, 0.0, -1.0))
    scene.CreateGravityMagnitudeAttr().Set(9.81)
    scene_prim = scene.GetPrim()
    scene_prim.CreateAttribute(
        'mjc:option:timestep', Sdf.ValueTypeNames.Double).Set(1.0 / 48.0)
    scene_prim.CreateAttribute(
        'mjc:option:iterations', Sdf.ValueTypeNames.Int).Set(50)

    UsdGeom.Xform.Define(stage, '/World/Links')
    world_body = UsdGeom.Xform.Define(stage, '/World/Links/__world').GetPrim()
    world_api = UsdPhysics.RigidBodyAPI.Apply(world_body)
    world_api.CreateRigidBodyEnabledAttr(False)
    UsdPhysics.MassAPI.Apply(world_body)

    body = UsdGeom.Xform.Define(stage, '/World/Links/drop').GetPrim()
    UsdGeom.Xformable(body).AddTranslateOp().Set(Gf.Vec3d(0.0, 0.0, 5.0))
    body_api = UsdPhysics.RigidBodyAPI.Apply(body)
    body_api.CreateRigidBodyEnabledAttr(True)
    UsdPhysics.MassAPI.Apply(body)  # No authored physics:mass: use Blender's default.
    collider = UsdGeom.Cube.Define(stage, '/World/Links/drop/Geom').GetPrim()
    UsdPhysics.CollisionAPI.Apply(collider)
    stage.GetRootLayer().Save()


def _check_default_mass_dynamics(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 48
    body = next(obj for obj in bpy.data.objects
                if obj.rigid_body and obj.rigid_body.type == 'ACTIVE')
    _assert_close(body.rigid_body.mass, 1.0)
    assert scene.rigidbody_world

    positions = {}
    for frame in range(1, 14):
        scene.frame_set(frame)
        if frame in (1, 13):
            bpy.context.view_layer.update()
            positions[frame] = float(body.evaluated_get(
                bpy.context.evaluated_depsgraph_get()
            ).matrix_world.translation.z)
    assert positions[13] < positions[1] - 0.1, positions
    print('BLENDER_USD_DEFAULT_MASS_DYNAMICS_PASS', positions)


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


def _check_imported_scaled_dynamics(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.gravity = (0.0, 0.0, -9.81)
    scene.render.fps = 60
    scene.render.fps_base = 1.25
    scene.frame_start = 1
    scene.frame_end = 120
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, -0.5))
    floor = bpy.context.object
    floor.name = 'ScaleCheckFloor'
    floor.dimensions = (20.0, 20.0, 1.0)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bpy.ops.rigidbody.object_add(type='PASSIVE')
    floor.rigid_body.collision_shape = 'BOX'
    floor.rigid_body.friction = 0.7

    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 2.0))
    source_body = bpy.context.object
    source_body.name = 'ScaleCheckBody'
    source_body.scale = (2.0, 0.5, 1.5)
    bpy.ops.rigidbody.object_add(type='ACTIVE')
    source_body.rigid_body.collision_shape = 'CONVEX_HULL'
    source_body.rigid_body.mass = 2.5
    source_body.rigid_body.friction = 0.7
    source_body.rigid_body.restitution = 0.0

    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 120

    export_result = bpy.ops.wm.usd_export(
        filepath=str(path), export_custom_properties=True,
        export_materials=False, export_animation=False,
    )
    assert 'FINISHED' in export_result, export_result

    def trajectory(body):
        positions = {}
        for frame in range(1, 121):
            scene.frame_set(frame)
            if frame in (1, 30, 60, 120):
                bpy.context.view_layer.update()
                positions[frame] = float(body.evaluated_get(
                    bpy.context.evaluated_depsgraph_get()
                ).matrix_world.translation.z)
        return positions

    source_positions = trajectory(source_body)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 48
    _assert_close(scene.render.fps_base, 1.0)
    active_bodies = [obj for obj in bpy.data.objects
                     if obj.rigid_body and obj.rigid_body.type == 'ACTIVE']
    assert len(active_bodies) == 1, active_bodies
    body = active_bodies[0]
    for actual, expected in zip(body.matrix_world.to_scale(), (2.0, 0.5, 1.5)):
        _assert_close(actual, expected)

    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 120
    imported_positions = trajectory(body)

    # The scaled cube is 1.5 Blender units tall; the floor top is z=0.
    assert 0.68 <= imported_positions[120] <= 0.84, imported_positions
    print('BLENDER_SOURCE_IMPORTED_TRAJECTORIES', source_positions,
          imported_positions)
    for frame, expected in source_positions.items():
        assert abs(imported_positions[frame] - expected) <= 0.02, (
            frame, imported_positions[frame], expected)
    print('BLENDER_IMPORTED_SCALE_DYNAMICS_PASS',
          {'scale': tuple(body.matrix_world.to_scale()),
           'source_z': source_positions, 'imported_z': imported_positions})


def main():
    spec = importlib.util.spec_from_file_location('lightusd_usd_physics_hook', HOOK_PATH)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)
    assert hook._HAS_PXR and hasattr(bpy.types, 'USDHook')

    fd, path_string = tempfile.mkstemp(suffix='.usda', prefix='lightusd-physics-')
    os.close(fd)
    path = Path(path_string)
    path.unlink()
    collision_groups_path = path.with_name(path.stem + '-groups.usda')
    multi_mesh_path = path.with_name(path.stem + '-multi-mesh.usda')
    default_mass_path = path.with_name(path.stem + '-default-mass.usda')
    scale_dynamics_path = path.with_name(path.stem + '-scale-dynamics.usda')
    friction_path = path.with_name(path.stem + '-mujoco-friction.usda')
    try:
        hook.register()
        _make_scene_and_export(path)
        _check_import(path, merge_parent_xform=True)
        _check_import(path, merge_parent_xform=False)
        _make_collision_group_fixture(collision_groups_path)
        _check_collision_group_import(collision_groups_path)
        _make_multi_mesh_fixture(multi_mesh_path)
        _check_multi_mesh_import(multi_mesh_path)
        _make_default_mass_fixture(default_mass_path)
        _check_default_mass_dynamics(default_mass_path)
        _make_mjcf_friction_fixture(friction_path)
        _check_mjcf_friction_dynamics(friction_path)
        _check_imported_scaled_dynamics(scale_dynamics_path)
        hook.unregister()
        _check_bullet_dynamics()
    finally:
        if path.exists():
            path.unlink()
        if collision_groups_path.exists():
            collision_groups_path.unlink()
        if multi_mesh_path.exists():
            multi_mesh_path.unlink()
        if default_mass_path.exists():
            default_mass_path.unlink()
        if scale_dynamics_path.exists():
            scale_dynamics_path.unlink()
        if friction_path.exists():
            friction_path.unlink()
        try:
            hook.unregister()
        except RuntimeError:
            pass

    print('BLENDER_USD_PHYSICS_RUNTIME_PASS', bpy.app.version_string)


if __name__ == '__main__':
    main()
