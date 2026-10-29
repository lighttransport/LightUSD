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


def _make_hinge_dynamics_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    scene = UsdPhysics.Scene.Define(stage, '/World/PhysicsScene')
    scene.CreateGravityDirectionAttr().Set(Gf.Vec3f(0.0, 0.0, -1.0))
    scene.CreateGravityMagnitudeAttr().Set(9.81)
    scene.GetPrim().CreateAttribute(
        'mjc:option:timestep', Sdf.ValueTypeNames.Double).Set(1.0 / 60.0)
    scene.GetPrim().CreateAttribute(
        'mjc:option:iterations', Sdf.ValueTypeNames.Int).Set(50)

    anchor = UsdGeom.Xform.Define(stage, '/World/Anchor').GetPrim()
    anchor_api = UsdPhysics.RigidBodyAPI.Apply(anchor)
    anchor_api.CreateRigidBodyEnabledAttr(False)
    UsdPhysics.MassAPI.Apply(anchor)
    anchor_geom = UsdGeom.Cube.Define(stage, '/World/Anchor/Geom')
    anchor_geom.CreateSizeAttr(0.002)
    UsdPhysics.CollisionAPI.Apply(anchor_geom.GetPrim())

    pendulum = UsdGeom.Xform.Define(stage, '/World/Pendulum').GetPrim()
    UsdGeom.Xformable(pendulum).AddTranslateOp().Set(Gf.Vec3d(1.0, 0.0, 0.0))
    UsdPhysics.RigidBodyAPI.Apply(pendulum)
    UsdPhysics.MassAPI.Apply(pendulum).CreateMassAttr(1.0)
    rod = UsdGeom.Capsule.Define(stage, '/World/Pendulum/Rod')
    rod.CreateAxisAttr().Set(UsdGeom.Tokens.x)
    rod.CreateHeightAttr(1.9)
    rod.CreateRadiusAttr(0.05)
    UsdPhysics.CollisionAPI.Apply(rod.GetPrim())

    hinge = UsdPhysics.RevoluteJoint.Define(stage, '/World/Joints/Hinge')
    hinge.CreateBody0Rel().SetTargets([anchor.GetPath()])
    hinge.CreateBody1Rel().SetTargets([pendulum.GetPath()])
    hinge.CreateAxisAttr().Set('Y')
    hinge.CreateLocalPos0Attr().Set(Gf.Vec3f(0.0, 0.0, 0.0))
    hinge.CreateLocalRot0Attr().Set(Gf.Quatf(1.0))
    hinge.CreateLocalPos1Attr().Set(Gf.Vec3f(-1.0, 0.0, 0.0))
    hinge.CreateLocalRot1Attr().Set(Gf.Quatf(1.0))
    stage.GetRootLayer().Save()


def _check_hinge_dynamics(path):
    from mathutils import Vector

    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 60
    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 121

    body = next(obj for obj in bpy.data.objects
                if obj.rigid_body and obj.rigid_body.type == 'ACTIVE')
    hinge = next(obj for obj in bpy.data.objects if obj.rigid_body_constraint)
    assert hinge.rigid_body_constraint.type == 'HINGE'
    assert hinge.rigid_body_constraint.object1
    assert hinge.rigid_body_constraint.object2 == body

    samples = {}
    for frame in range(1, 122):
        scene.frame_set(frame)
        if frame not in (1, 31, 61, 91, 121):
            continue
        bpy.context.view_layer.update()
        evaluated = body.evaluated_get(bpy.context.evaluated_depsgraph_get())
        matrix = evaluated.matrix_world
        position = matrix.translation.copy()
        pivot = position + matrix.to_3x3() @ Vector((-1.0, 0.0, 0.0))
        rotation = matrix.to_quaternion().copy()
        samples[frame] = {
            'position': tuple(float(value) for value in position),
            'pivot': tuple(float(value) for value in pivot),
            'rotation': tuple(float(value) for value in rotation),
        }

    assert samples[1]['position'] == (1.0, 0.0, 0.0), samples
    assert abs(samples[61]['rotation'][2]) > 0.2, samples
    for frame, sample in samples.items():
        assert all(math.isfinite(value) for value in sample['position'] +
                   sample['pivot'] + sample['rotation']), (frame, sample)
        assert math.sqrt(sum(value * value for value in sample['pivot'])) < 0.05, (
            frame, sample)
    print('BLENDER_USD_HINGE_DYNAMICS_PASS', samples)


def _make_offdiagonal_inertia_dynamics_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    scene = UsdPhysics.Scene.Define(stage, '/World/PhysicsScene')
    scene.CreateGravityDirectionAttr().Set(Gf.Vec3f(0.0, 0.0, -1.0))
    scene.CreateGravityMagnitudeAttr().Set(9.81)
    scene.GetPrim().CreateAttribute(
        'mjc:option:timestep', Sdf.ValueTypeNames.Double).Set(1.0 / 60.0)
    scene.GetPrim().CreateAttribute(
        'mjc:option:iterations', Sdf.ValueTypeNames.Int).Set(50)

    anchor = UsdGeom.Xform.Define(stage, '/World/Anchor').GetPrim()
    UsdPhysics.RigidBodyAPI.Apply(anchor).CreateRigidBodyEnabledAttr(False)
    UsdPhysics.MassAPI.Apply(anchor)
    anchor_geom = UsdGeom.Cube.Define(stage, '/World/Anchor/Geom')
    anchor_geom.CreateSizeAttr(0.002)
    UsdPhysics.CollisionAPI.Apply(anchor_geom.GetPrim())

    bar = UsdGeom.Xform.Define(stage, '/World/Bar').GetPrim()
    UsdGeom.Xformable(bar).AddTranslateOp().Set(Gf.Vec3d(1.0, 0.0, 0.0))
    UsdPhysics.RigidBodyAPI.Apply(bar)
    mass = UsdPhysics.MassAPI.Apply(bar)
    mass.CreateMassAttr(1.0)
    mass.CreateCenterOfMassAttr(Gf.Vec3f(0.0, 0.0, 0.0))
    mass.CreateDiagonalInertiaAttr(Gf.Vec3f(0.65, 0.35, 0.8))
    mass.CreatePrincipalAxesAttr(Gf.Quatf(
        math.cos(math.pi / 8), Gf.Vec3f(0.0, 0.0, -math.sin(math.pi / 8))))

    # This box has principal moments (0.35, 0.65, 0.8), rotated 45 degrees
    # about Z. Its body-frame tensor therefore has Ixy = -0.15.
    half_extents = (math.sqrt(1.65), math.sqrt(0.75), math.sqrt(0.3))
    points = []
    for x, y, z in ((-half_extents[0], -half_extents[1], -half_extents[2]),
                    (half_extents[0], -half_extents[1], -half_extents[2]),
                    (half_extents[0], half_extents[1], -half_extents[2]),
                    (-half_extents[0], half_extents[1], -half_extents[2]),
                    (-half_extents[0], -half_extents[1], half_extents[2]),
                    (half_extents[0], -half_extents[1], half_extents[2]),
                    (half_extents[0], half_extents[1], half_extents[2]),
                    (-half_extents[0], half_extents[1], half_extents[2])):
        points.append(Gf.Vec3f((x - y) / math.sqrt(2.0),
                               (x + y) / math.sqrt(2.0), z))
    mesh = UsdGeom.Mesh.Define(stage, '/World/Bar/Box')
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr([4] * 6)
    mesh.CreateFaceVertexIndicesAttr(
        [0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 1, 2, 6, 5,
         2, 3, 7, 6, 3, 0, 4, 7])
    UsdPhysics.CollisionAPI.Apply(mesh.GetPrim())

    axis_angle = math.atan2(1.0, 2.0)
    frame_rotation = Gf.Quatf(math.cos(axis_angle / 2.0),
                              Gf.Vec3f(0.0, 0.0, math.sin(axis_angle / 2.0)))
    hinge = UsdPhysics.RevoluteJoint.Define(stage, '/World/Joints/Hinge')
    hinge.CreateBody0Rel().SetTargets([anchor.GetPath()])
    hinge.CreateBody1Rel().SetTargets([bar.GetPath()])
    hinge.CreateAxisAttr().Set('X')
    hinge.CreateLocalPos0Attr().Set(Gf.Vec3f(0.0, 0.0, 0.0))
    hinge.CreateLocalRot0Attr().Set(frame_rotation)
    hinge.CreateLocalPos1Attr().Set(Gf.Vec3f(-1.0, 0.0, 0.0))
    hinge.CreateLocalRot1Attr().Set(frame_rotation)

    slider_anchor = UsdGeom.Xform.Define(stage, '/World/SliderAnchor').GetPrim()
    UsdGeom.Xformable(slider_anchor).AddTranslateOp().Set(Gf.Vec3d(3.0, 0.0, 0.0))
    UsdPhysics.RigidBodyAPI.Apply(slider_anchor).CreateRigidBodyEnabledAttr(False)
    UsdPhysics.MassAPI.Apply(slider_anchor)
    slider_anchor_geom = UsdGeom.Cube.Define(
        stage, '/World/SliderAnchor/AnchorGeom')
    slider_anchor_geom.CreateSizeAttr(0.002)
    UsdPhysics.CollisionAPI.Apply(slider_anchor_geom.GetPrim())
    slider = UsdGeom.Xform.Define(stage, '/World/Slider').GetPrim()
    UsdGeom.Xformable(slider).AddTranslateOp().Set(Gf.Vec3d(3.0, 0.0, 0.0))
    UsdPhysics.RigidBodyAPI.Apply(slider)
    UsdPhysics.MassAPI.Apply(slider).CreateMassAttr(1.0)
    slider_geom = UsdGeom.Cube.Define(stage, '/World/Slider/SliderGeom')
    slider_geom.CreateSizeAttr(0.1)
    UsdPhysics.CollisionAPI.Apply(slider_geom.GetPrim())
    slider_joint = UsdPhysics.PrismaticJoint.Define(
        stage, '/World/Joints/SliderJoint')
    slider_joint.CreateBody0Rel().SetTargets([slider_anchor.GetPath()])
    slider_joint.CreateBody1Rel().SetTargets([slider.GetPath()])
    slider_joint.CreateAxisAttr().Set('Z')
    slider_joint.CreateLocalPos0Attr().Set(Gf.Vec3f(0.0, 0.0, 0.0))
    slider_joint.CreateLocalRot0Attr().Set(Gf.Quatf(1.0))
    slider_joint.CreateLocalPos1Attr().Set(Gf.Vec3f(0.0, 0.0, 0.0))
    slider_joint.CreateLocalRot1Attr().Set(Gf.Quatf(1.0))
    stage.GetRootLayer().Save()


def _check_offdiagonal_inertia_dynamics(path):
    from mathutils import Vector

    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 60
    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 121

    body = bpy.data.objects['Box']
    assert body.rigid_body and body.rigid_body.type == 'ACTIVE'
    hinge_obj = next(obj for obj in bpy.data.objects
                     if obj.rigid_body_constraint and
                     obj.rigid_body_constraint.type == 'HINGE')
    hinge = hinge_obj.rigid_body_constraint
    assert hinge.type == 'HINGE'
    assert hinge.object1 and hinge.object2 == body
    # The USD frame maps its local X axis to this world axis. Blender's local Z
    # hinge axis must be reoriented by the import hook to match it.
    expected_axis = Vector((2.0 / math.sqrt(5.0), 1.0 / math.sqrt(5.0), 0.0))
    actual_axis = hinge_obj.matrix_world.to_3x3() @ Vector((0.0, 0.0, 1.0))
    assert (actual_axis - expected_axis).length < 1e-5, (actual_axis, expected_axis)
    slider_obj = next(obj for obj in bpy.data.objects
                      if obj.rigid_body_constraint and
                      obj.rigid_body_constraint.type == 'SLIDER')
    slider_axis = (slider_obj.matrix_world.to_3x3() @ Vector((1.0, 0.0, 0.0)))
    assert (slider_axis - Vector((0.0, 0.0, 1.0))).length < 1e-5, slider_axis

    samples = {}
    for frame in range(1, 122):
        scene.frame_set(frame)
        if frame not in (1, 31, 61, 91, 121):
            continue
        bpy.context.view_layer.update()
        evaluated = body.evaluated_get(bpy.context.evaluated_depsgraph_get())
        matrix = evaluated.matrix_world
        position = matrix.translation.copy()
        pivot = position + matrix.to_3x3() @ Vector((-1.0, 0.0, 0.0))
        samples[frame] = {
            'position': tuple(float(value) for value in position),
            'pivot': tuple(float(value) for value in pivot),
            'rotation': tuple(float(value) for value in matrix.to_quaternion()),
        }

    assert samples[1]['position'] == (1.0, 0.0, 0.0), samples
    assert max(abs(value) for value in samples[61]['rotation'][1:]) > 0.1, samples
    for frame, sample in samples.items():
        assert all(math.isfinite(value) for value in sample['position'] +
                   sample['pivot'] + sample['rotation']), (frame, sample)
        assert math.sqrt(sum(value * value for value in sample['pivot'])) < 0.01, (
            frame, sample)
    print('BLENDER_USD_OFFDIAGONAL_INERTIA_DYNAMICS_PASS', samples)


def _make_two_link_dynamics_fixture(path):
    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(stage, 1.0)
    world = UsdGeom.Xform.Define(stage, '/World')
    stage.SetDefaultPrim(world.GetPrim())
    scene = UsdPhysics.Scene.Define(stage, '/World/PhysicsScene')
    scene.CreateGravityDirectionAttr().Set(Gf.Vec3f(0.0, 0.0, -1.0))
    scene.CreateGravityMagnitudeAttr().Set(9.81)
    scene.GetPrim().CreateAttribute(
        'mjc:option:timestep', Sdf.ValueTypeNames.Double).Set(1.0 / 60.0)
    scene.GetPrim().CreateAttribute(
        'mjc:option:iterations', Sdf.ValueTypeNames.Int).Set(50)

    def rigid_body(name, x, geometry_name, active, collision_mask):
        prim = UsdGeom.Xform.Define(stage, '/World/' + name).GetPrim()
        UsdGeom.Xformable(prim).AddTranslateOp().Set(Gf.Vec3d(x, 0.0, 0.0))
        body_api = UsdPhysics.RigidBodyAPI.Apply(prim)
        body_api.CreateRigidBodyEnabledAttr(active)
        UsdPhysics.MassAPI.Apply(prim).CreateMassAttr(1.0)
        prim.CreateAttribute('userProperties:blender:collisionMask',
                             Sdf.ValueTypeNames.Int).Set(collision_mask)
        if active:
            geom = UsdGeom.Capsule.Define(
                stage, f'/World/{name}/{geometry_name}')
            geom.CreateAxisAttr().Set(UsdGeom.Tokens.x)
            geom.CreateHeightAttr(1.9)
            geom.CreateRadiusAttr(0.05)
        else:
            geom = UsdGeom.Cube.Define(
                stage, f'/World/{name}/{geometry_name}')
            geom.CreateSizeAttr(0.002)
        UsdPhysics.CollisionAPI.Apply(geom.GetPrim())
        return prim

    anchor = rigid_body('Anchor', 0.0, 'AnchorGeom', False, 1)
    link1 = rigid_body('Link1', 1.0, 'Link1Rod', True, 2)
    link2 = rigid_body('Link2', 3.1, 'Link2Rod', True, 4)
    def hinge(name, body0, body1, local_pos0, local_pos1):
        joint = UsdPhysics.RevoluteJoint.Define(stage, '/World/Joints/' + name)
        joint.CreateBody0Rel().SetTargets([body0.GetPath()])
        joint.CreateBody1Rel().SetTargets([body1.GetPath()])
        joint.CreateAxisAttr().Set('Y')
        joint.CreateLocalPos0Attr().Set(Gf.Vec3f(*local_pos0))
        joint.CreateLocalRot0Attr().Set(Gf.Quatf(1.0))
        joint.CreateLocalPos1Attr().Set(Gf.Vec3f(*local_pos1))
        joint.CreateLocalRot1Attr().Set(Gf.Quatf(1.0))

    hinge('Hinge1', anchor, link1, (0.0, 0.0, 0.0), (-1.0, 0.0, 0.0))
    hinge('Hinge2', link1, link2, (1.1, 0.0, 0.0), (-1.0, 0.0, 0.0))
    stage.GetRootLayer().Save()


def _check_two_link_dynamics(path):
    from mathutils import Vector

    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 60
    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 121

    anchor = bpy.data.objects['AnchorGeom']
    link1 = bpy.data.objects['Link1Rod']
    link2 = bpy.data.objects['Link2Rod']
    assert anchor.rigid_body and anchor.rigid_body.type == 'PASSIVE'
    assert link1.rigid_body and link1.rigid_body.type == 'ACTIVE'
    assert link2.rigid_body and link2.rigid_body.type == 'ACTIVE'
    hinges = {obj.name: obj.rigid_body_constraint for obj in bpy.data.objects
              if obj.rigid_body_constraint}
    assert hinges['Hinge1'].type == 'HINGE'
    assert hinges['Hinge1'].object1 == anchor and hinges['Hinge1'].object2 == link1
    assert hinges['Hinge2'].type == 'HINGE'
    assert hinges['Hinge2'].object1 == link1 and hinges['Hinge2'].object2 == link2

    samples = {}
    for frame in range(1, 122):
        scene.frame_set(frame)
        if frame not in (1, 31, 61, 91, 121):
            continue
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        a, b = link1.evaluated_get(depsgraph), link2.evaluated_get(depsgraph)
        ma, mb = a.matrix_world, b.matrix_world
        pa, pb = ma.translation.copy(), mb.translation.copy()
        qa, qb = ma.to_quaternion().copy(), mb.to_quaternion().copy()
        pivot1 = pa + ma.to_3x3() @ Vector((-1.0, 0.0, 0.0))
        pivot2_a = pa + ma.to_3x3() @ Vector((1.1, 0.0, 0.0))
        pivot2_b = pb + mb.to_3x3() @ Vector((-1.0, 0.0, 0.0))
        samples[frame] = {
            'link1': tuple(float(v) for v in pa),
            'link2': tuple(float(v) for v in pb),
            'pivot1': tuple(float(v) for v in pivot1),
            'pivot2_parent': tuple(float(v) for v in pivot2_a),
            'pivot2_child': tuple(float(v) for v in pivot2_b),
            'rotation1_y': float(qa.y),
            'rotation2_y': float(qb.y),
        }

    assert samples[1]['link1'] == (1.0, 0.0, 0.0), samples
    assert all(abs(actual - expected) < 1e-4 for actual, expected in
               zip(samples[1]['link2'], (3.1, 0.0, 0.0))), samples
    assert abs(samples[61]['rotation1_y']) > 0.2, samples
    assert abs(samples[61]['rotation2_y']) > 0.2, samples
    for frame, sample in samples.items():
        values = (sample['link1'] + sample['link2'] + sample['pivot1'] +
                  sample['pivot2_parent'] + sample['pivot2_child'] +
                  (sample['rotation1_y'], sample['rotation2_y']))
        assert all(math.isfinite(value) for value in values), (frame, sample)
        assert math.sqrt(sum(v * v for v in sample['pivot1'])) < 0.01, (
            frame, sample)
        mismatch = math.sqrt(sum(
            (sample['pivot2_parent'][axis] - sample['pivot2_child'][axis]) ** 2
            for axis in range(3)))
        assert mismatch < 0.01, (frame, sample)
    print('BLENDER_USD_TWO_LINK_DYNAMICS_PASS', samples)


def _make_three_link_dynamics_fixture(path):
    _make_two_link_dynamics_fixture(path)
    stage = Usd.Stage.Open(str(path))
    link2 = stage.GetPrimAtPath('/World/Link2')

    link3 = UsdGeom.Xform.Define(stage, '/World/Link3').GetPrim()
    UsdGeom.Xformable(link3).AddTranslateOp().Set(Gf.Vec3d(5.2, 0.0, 0.0))
    UsdPhysics.RigidBodyAPI.Apply(link3)
    UsdPhysics.MassAPI.Apply(link3).CreateMassAttr(1.0)
    link3.CreateAttribute('userProperties:blender:collisionMask',
                          Sdf.ValueTypeNames.Int).Set(8)
    rod = UsdGeom.Capsule.Define(stage, '/World/Link3/Link3Rod')
    rod.CreateAxisAttr().Set(UsdGeom.Tokens.x)
    rod.CreateHeightAttr(1.9)
    rod.CreateRadiusAttr(0.05)
    UsdPhysics.CollisionAPI.Apply(rod.GetPrim())

    hinge = UsdPhysics.RevoluteJoint.Define(stage, '/World/Joints/Hinge3')
    hinge.CreateBody0Rel().SetTargets([link2.GetPath()])
    hinge.CreateBody1Rel().SetTargets([link3.GetPath()])
    hinge.CreateAxisAttr().Set('Y')
    hinge.CreateLocalPos0Attr().Set(Gf.Vec3f(1.1, 0.0, 0.0))
    hinge.CreateLocalRot0Attr().Set(Gf.Quatf(1.0))
    hinge.CreateLocalPos1Attr().Set(Gf.Vec3f(-1.0, 0.0, 0.0))
    hinge.CreateLocalRot1Attr().Set(Gf.Quatf(1.0))
    stage.GetRootLayer().Save()


def _check_three_link_dynamics(path):
    from mathutils import Vector

    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    assert scene.render.fps == 60
    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 121

    anchor = bpy.data.objects['AnchorGeom']
    links = [bpy.data.objects[f'Link{index}Rod'] for index in range(1, 4)]
    assert anchor.rigid_body and anchor.rigid_body.type == 'PASSIVE'
    assert all(link.rigid_body and link.rigid_body.type == 'ACTIVE' for link in links)
    hinges = {obj.name: obj.rigid_body_constraint for obj in bpy.data.objects
              if obj.rigid_body_constraint}
    expected_bodies = [('Hinge1', anchor, links[0]),
                       ('Hinge2', links[0], links[1]),
                       ('Hinge3', links[1], links[2])]
    for name, body0, body1 in expected_bodies:
        constraint = hinges[name]
        assert constraint.type == 'HINGE'
        assert constraint.object1 == body0 and constraint.object2 == body1

    samples = {}
    for frame in range(1, 122):
        scene.frame_set(frame)
        if frame not in (1, 31, 61, 91, 121):
            continue
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = [link.evaluated_get(depsgraph) for link in links]
        matrices = [obj.matrix_world.copy() for obj in evaluated]
        positions = [matrix.translation.copy() for matrix in matrices]
        pivots = [
            positions[0] + matrices[0].to_3x3() @ Vector((-1.0, 0.0, 0.0)),
            positions[0] + matrices[0].to_3x3() @ Vector((1.1, 0.0, 0.0)),
            positions[1] + matrices[1].to_3x3() @ Vector((-1.0, 0.0, 0.0)),
            positions[1] + matrices[1].to_3x3() @ Vector((1.1, 0.0, 0.0)),
            positions[2] + matrices[2].to_3x3() @ Vector((-1.0, 0.0, 0.0)),
        ]
        samples[frame] = {
            'positions': [tuple(float(v) for v in position) for position in positions],
            'pivots': [tuple(float(v) for v in pivot) for pivot in pivots],
            'rotations': [tuple(float(v) for v in matrix.to_quaternion())
                          for matrix in matrices],
        }

    for index, expected_x in enumerate((1.0, 3.1, 5.2)):
        _assert_close(samples[1]['positions'][index][0], expected_x, 1e-4)
    assert all(abs(rotation[2]) > 0.2
               for rotation in samples[61]['rotations']), samples
    for frame, sample in samples.items():
        flattened = [value for vector in sample['positions'] + sample['pivots'] +
                     sample['rotations'] for value in vector]
        assert all(math.isfinite(value) for value in flattened), (frame, sample)
        assert math.sqrt(sum(value * value for value in sample['pivots'][0])) < 0.01, (
            frame, sample)
        for parent_index, child_index in ((1, 2), (3, 4)):
            mismatch = math.sqrt(sum(
                (sample['pivots'][parent_index][axis] -
                 sample['pivots'][child_index][axis]) ** 2
                for axis in range(3)))
            assert mismatch < 0.01, (frame, sample)
    print('BLENDER_USD_THREE_LINK_DYNAMICS_PASS', samples)


def _make_n_link_dynamics_fixture(path, link_count):
    assert link_count >= 4
    _make_three_link_dynamics_fixture(path)
    stage = Usd.Stage.Open(str(path))
    for index in range(4, link_count + 1):
        previous = stage.GetPrimAtPath(f'/World/Link{index - 1}')
        link = UsdGeom.Xform.Define(stage, f'/World/Link{index}').GetPrim()
        UsdGeom.Xformable(link).AddTranslateOp().Set(
            Gf.Vec3d(1.0 + 2.1 * (index - 1), 0.0, 0.0))
        UsdPhysics.RigidBodyAPI.Apply(link)
        UsdPhysics.MassAPI.Apply(link).CreateMassAttr(1.0)
        link.CreateAttribute('userProperties:blender:collisionMask',
                              Sdf.ValueTypeNames.Int).Set(1 << index)
        rod = UsdGeom.Capsule.Define(stage, f'/World/Link{index}/Link{index}Rod')
        rod.CreateAxisAttr().Set(UsdGeom.Tokens.x)
        rod.CreateHeightAttr(1.9)
        rod.CreateRadiusAttr(0.05)
        UsdPhysics.CollisionAPI.Apply(rod.GetPrim())

        hinge = UsdPhysics.RevoluteJoint.Define(stage, f'/World/Joints/Hinge{index}')
        hinge.CreateBody0Rel().SetTargets([previous.GetPath()])
        hinge.CreateBody1Rel().SetTargets([link.GetPath()])
        hinge.CreateAxisAttr().Set('Y')
        hinge.CreateLocalPos0Attr().Set(Gf.Vec3f(1.1, 0.0, 0.0))
        hinge.CreateLocalRot0Attr().Set(Gf.Quatf(1.0))
        hinge.CreateLocalPos1Attr().Set(Gf.Vec3f(-1.0, 0.0, 0.0))
        hinge.CreateLocalRot1Attr().Set(Gf.Quatf(1.0))
    stage.GetRootLayer().Save()


def _make_four_link_dynamics_fixture(path):
    _make_n_link_dynamics_fixture(path, 4)


def _check_n_link_dynamics(path, link_count, label):
    from mathutils import Vector

    bpy.ops.wm.read_factory_settings(use_empty=True)
    result = bpy.ops.wm.usd_import(filepath=str(path), merge_parent_xform=True)
    assert 'FINISHED' in result, result
    scene = bpy.context.scene
    scene.render.fps = 60
    world = scene.rigidbody_world
    assert world
    world.substeps_per_frame = 10
    world.solver_iterations = 50
    world.point_cache.frame_start = 1
    world.point_cache.frame_end = 121

    anchor = bpy.data.objects['AnchorGeom']
    links = [bpy.data.objects[f'Link{index}Rod']
             for index in range(1, link_count + 1)]
    assert anchor.rigid_body and anchor.rigid_body.type == 'PASSIVE'
    assert all(link.rigid_body and link.rigid_body.type == 'ACTIVE' for link in links)
    hinges = {obj.name: obj.rigid_body_constraint for obj in bpy.data.objects
              if obj.rigid_body_constraint}
    expected_bodies = [(f'Hinge{index}', anchor if index == 1 else links[index - 2],
                        links[index - 1])
                       for index in range(1, link_count + 1)]
    for name, body0, body1 in expected_bodies:
        constraint = hinges[name]
        assert constraint.type == 'HINGE'
        assert constraint.object1 == body0 and constraint.object2 == body1

    samples = {}
    for frame in range(1, 122):
        scene.frame_set(frame)
        if frame not in (1, 31, 61, 91, 121):
            continue
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = [link.evaluated_get(depsgraph) for link in links]
        matrices = [obj.matrix_world.copy() for obj in evaluated]
        positions = [matrix.translation.copy() for matrix in matrices]
        pivots = [positions[0] + matrices[0].to_3x3() @ Vector((-1.0, 0.0, 0.0))]
        for index in range(link_count - 1):
            pivots.append(positions[index] + matrices[index].to_3x3() @
                          Vector((1.1, 0.0, 0.0)))
            pivots.append(positions[index + 1] + matrices[index + 1].to_3x3() @
                          Vector((-1.0, 0.0, 0.0)))
        samples[frame] = {
            'positions': [tuple(float(v) for v in position) for position in positions],
            'pivots': [tuple(float(v) for v in pivot) for pivot in pivots],
            'rotations': [tuple(float(v) for v in matrix.to_quaternion())
                          for matrix in matrices],
        }

    for index in range(link_count):
        expected_x = 1.0 + 2.1 * index
        _assert_close(samples[1]['positions'][index][0], expected_x, 1e-4)
    assert all(position[2] < -0.1
               for position in samples[31]['positions']), samples
    assert all(abs(rotation[2]) > 0.2
               for rotation in samples[121]['rotations']), samples
    for frame, sample in samples.items():
        flattened = [value for vector in sample['positions'] + sample['pivots'] +
                     sample['rotations'] for value in vector]
        assert all(math.isfinite(value) for value in flattened), (frame, sample)
        assert math.sqrt(sum(value * value for value in sample['pivots'][0])) < 0.01, (
            frame, sample)
        for hinge_index in range(link_count - 1):
            parent_index, child_index = (1 + 2 * hinge_index,
                                         2 + 2 * hinge_index)
            mismatch = math.sqrt(sum(
                (sample['pivots'][parent_index][axis] -
                 sample['pivots'][child_index][axis]) ** 2
                for axis in range(3)))
            assert mismatch < 0.01, (frame, sample)
    print(f'BLENDER_USD_{label}_LINK_DYNAMICS_PASS', samples)


def _check_four_link_dynamics(path):
    _check_n_link_dynamics(path, 4, 'FOUR')


def _check_eight_link_dynamics(path):
    _check_n_link_dynamics(path, 8, 'EIGHT')


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


def _check_timing_and_limit_runtime(hook):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    bpy.ops.rigidbody.world_add()
    for timestep in (1.001 / 60, 0.003, 1 / 48):
        hook._apply_physics_to_blender({
            'scene': {'mjc': {'timestep': timestep}}, 'bodies': [], 'joints': []})
        _assert_close(scene.render.fps_base / scene.render.fps, timestep, 1e-8)
    print('BLENDER_USD_FRACTIONAL_TIMESTEP_PASS')
    scene.gravity = (0, 0, 0)
    scene.render.fps = 60
    scene.render.fps_base = 1
    bpy.ops.mesh.primitive_cube_add(size=0.5)
    body = bpy.context.object
    body.name = 'LimitBody'
    bpy.ops.rigidbody.object_add()
    body.rigid_body.angular_damping = 0
    bpy.ops.mesh.primitive_cube_add(size=0.1)
    anchor = bpy.context.object
    anchor.name = 'LimitAnchor'
    bpy.ops.rigidbody.object_add(type='PASSIVE')
    stage = Usd.Stage.CreateInMemory()
    stage.DefinePrim('/LimitBody', 'Cube')
    stage.DefinePrim('/LimitAnchor', 'Cube')
    cases = [('UnlimitedHinge', UsdPhysics.RevoluteJoint, -math.inf, math.inf),
             ('BoundedHinge', UsdPhysics.RevoluteJoint, -45, 60),
             ('UnlimitedSlider', UsdPhysics.PrismaticJoint, -math.inf, math.inf),
             ('BoundedSlider', UsdPhysics.PrismaticJoint, -0.5, 0.75),
             ('OneSidedHinge', UsdPhysics.RevoluteJoint, 0, math.inf),
             ('OutOfRangeHinge', UsdPhysics.RevoluteJoint, -500, 500)]
    for name, schema, low, high in cases:
        joint = schema.Define(stage, '/' + name)
        joint.CreateBody0Rel().SetTargets(['/LimitAnchor'])
        joint.CreateBody1Rel().SetTargets(['/LimitBody'])
        joint.CreateLocalPos0Attr(Gf.Vec3f(0))
        joint.CreateLocalPos1Attr(Gf.Vec3f(0))
        joint.CreateAxisAttr('Z' if schema == UsdPhysics.RevoluteJoint else 'X')
        joint.CreateLowerLimitAttr(low)
        joint.CreateUpperLimitAttr(high)
    d6 = UsdPhysics.Joint.Define(stage, '/LimitD6')
    d6.CreateBody0Rel().SetTargets(['/LimitAnchor'])
    d6.CreateBody1Rel().SetTargets(['/LimitBody'])
    for dof, low, high in [('transX', 1, -1), ('rotY', -math.inf, math.inf)]:
        limit = UsdPhysics.LimitAPI.Apply(d6.GetPrim(), dof)
        limit.CreateLowAttr(low)
        limit.CreateHighAttr(high)
    hook._apply_physics_to_blender(hook._read_usd_physics(stage, 'USD'))
    hinge = bpy.data.objects['UnlimitedHinge'].rigid_body_constraint
    slider = bpy.data.objects['UnlimitedSlider'].rigid_body_constraint
    assert not hinge.use_limit_ang_z and not slider.use_limit_lin_x
    bounded = bpy.data.objects['BoundedHinge'].rigid_body_constraint
    assert bounded.use_limit_ang_z
    _assert_close(bounded.limit_ang_z_lower, -math.pi / 4)
    _assert_close(bounded.limit_ang_z_upper, math.pi / 3)
    bounded_slider = bpy.data.objects['BoundedSlider'].rigid_body_constraint
    assert bounded_slider.use_limit_lin_x
    _assert_close(bounded_slider.limit_lin_x_lower, -0.5)
    _assert_close(bounded_slider.limit_lin_x_upper, 0.75)
    for name in ('OneSidedHinge', 'OutOfRangeHinge'):
        obj = bpy.data.objects.get(name)
        assert obj is None or obj.rigid_body_constraint is None
    constraint = bpy.data.objects['LimitD6'].rigid_body_constraint
    assert constraint.use_limit_lin_x and not constraint.use_limit_ang_y
    _assert_close(constraint.limit_lin_x_lower, 0)
    _assert_close(constraint.limit_lin_x_upper, 0)
    # Roundtrip the unlimited constraint through the USD schema mapping.
    hook._export_constraint(stage, bpy.data.objects['UnlimitedHinge'], 'USD')
    exported = stage.GetPrimAtPath('/Joints/UnlimitedHinge')
    assert exported.GetAttribute('physics:lowerLimit').Get() == -math.inf
    assert exported.GetAttribute('physics:upperLimit').Get() == math.inf
    for obj in bpy.data.objects:
        if obj.rigid_body_constraint:
            obj.rigid_body_constraint.enabled = obj.name == 'UnlimitedHinge'
            obj.rigid_body_constraint.disable_collisions = True
    # Apply a motor about the hinge's Z axis and unwrap the actual simulated
    # body angle. The imported hinge must permit more than a full revolution.
    bpy.ops.object.empty_add()
    motor_obj = bpy.context.object
    motor_obj.rotation_euler.y = math.pi / 2
    bpy.ops.rigidbody.constraint_add(type='MOTOR')
    motor = motor_obj.rigid_body_constraint
    motor.object1, motor.object2 = anchor, body
    motor.disable_collisions = True
    motor.use_motor_ang = True
    motor.motor_ang_target_velocity = 8
    motor.motor_ang_max_impulse = 100
    scene.rigidbody_world.substeps_per_frame = 10
    scene.rigidbody_world.solver_iterations = 50
    previous = total = 0
    for frame in range(1, 241):
        scene.frame_set(frame)
        matrix = body.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world
        angle = math.atan2(matrix[1][0], matrix[0][0])
        total += math.atan2(math.sin(angle - previous), math.cos(angle - previous))
        previous = angle
    assert abs(total) > 2 * math.pi, total
    print('BLENDER_USD_JOINT_LIMITS_PASS', {'unwrapped_radians': total})


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
    hinge_path = path.with_name(path.stem + '-hinge-dynamics.usda')
    offdiagonal_path = path.with_name(path.stem + '-offdiagonal-inertia.usda')
    two_link_path = path.with_name(path.stem + '-two-link-dynamics.usda')
    three_link_path = path.with_name(path.stem + '-three-link-dynamics.usda')
    four_link_path = path.with_name(path.stem + '-four-link-dynamics.usda')
    eight_link_path = path.with_name(path.stem + '-eight-link-dynamics.usda')
    try:
        hook.register()
        _check_timing_and_limit_runtime(hook)
        _make_scene_and_export(path)
        _check_import(path, merge_parent_xform=True)
        _check_import(path, merge_parent_xform=False)
        _make_collision_group_fixture(collision_groups_path)
        _check_collision_group_import(collision_groups_path)
        _make_multi_mesh_fixture(multi_mesh_path)
        _check_multi_mesh_import(multi_mesh_path)
        _make_default_mass_fixture(default_mass_path)
        _check_default_mass_dynamics(default_mass_path)
        _make_hinge_dynamics_fixture(hinge_path)
        _check_hinge_dynamics(hinge_path)
        _make_offdiagonal_inertia_dynamics_fixture(offdiagonal_path)
        _check_offdiagonal_inertia_dynamics(offdiagonal_path)
        _make_two_link_dynamics_fixture(two_link_path)
        _check_two_link_dynamics(two_link_path)
        _make_three_link_dynamics_fixture(three_link_path)
        _check_three_link_dynamics(three_link_path)
        _make_four_link_dynamics_fixture(four_link_path)
        _check_four_link_dynamics(four_link_path)
        _make_n_link_dynamics_fixture(eight_link_path, 8)
        _check_eight_link_dynamics(eight_link_path)
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
        if hinge_path.exists():
            hinge_path.unlink()
        if offdiagonal_path.exists():
            offdiagonal_path.unlink()
        if two_link_path.exists():
            two_link_path.unlink()
        if three_link_path.exists():
            three_link_path.unlink()
        if four_link_path.exists():
            four_link_path.unlink()
        if eight_link_path.exists():
            eight_link_path.unlink()
        try:
            hook.unregister()
        except RuntimeError:
            pass

    print('BLENDER_USD_PHYSICS_RUNTIME_PASS', bpy.app.version_string)


if __name__ == '__main__':
    main()
