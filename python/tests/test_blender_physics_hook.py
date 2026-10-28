# SPDX-License-Identifier: Apache-2.0
"""Exercise the addon's schema mapping with OpenUSD and a minimal bpy stub.

These tests do not replace a Blender integration run.
"""
import importlib.util
import math
import pathlib
import sys
from types import SimpleNamespace

import numpy as np
import pytest

pxr = pytest.importorskip('pxr')
from pxr import Gf, Sdf, Usd, UsdPhysics


class ZMatrix:
    """Small matrix stand-in for rigid Z rotations; Blender uses mathutils."""
    def __init__(self, angle=0.0, position=(0, 0, 0), matrix=None):
        c, s = math.cos(angle), math.sin(angle)
        self.matrix = np.array(matrix) if matrix is not None else np.array(
            [[c, -s, 0, position[0]], [s, c, 0, position[1]],
             [0, 0, 1, position[2]], [0, 0, 0, 1]], dtype=float)

    def inverted(self):
        try:
            return ZMatrix(matrix=np.linalg.inv(self.matrix))
        except np.linalg.LinAlgError as exc:
            raise ValueError('singular') from exc

    def __matmul__(self, other):
        return ZMatrix(matrix=self.matrix @ other.matrix)

    @property
    def translation(self):
        return self.matrix[:3, 3]

    def to_quaternion(self):
        angle = math.atan2(self.matrix[1, 0], self.matrix[0, 0])
        q = SimpleNamespace(w=math.cos(angle/2), x=0.0, y=0.0, z=math.sin(angle/2))
        q.normalized = lambda: q
        return q

    def to_4x4(self):
        return self


@pytest.fixture
def hook(monkeypatch):
    bpy = SimpleNamespace(
        types=SimpleNamespace(AddonPreferences=object, USDHook=object),
        props=SimpleNamespace(EnumProperty=lambda **kwargs: None),
    )
    monkeypatch.setitem(sys.modules, 'bpy', bpy)
    class Matrix:
        @staticmethod
        def Translation(position):
            return ZMatrix(position=position)
    class Quaternion:
        def __init__(self, value):
            self.value = value

        def to_matrix(self):
            return ZMatrix(2*math.atan2(self.value[3], self.value[0]))
    monkeypatch.setitem(sys.modules, 'mathutils', SimpleNamespace(Matrix=Matrix, Quaternion=Quaternion))
    path = pathlib.Path(__file__).resolve().parents[2] / 'blender/usd_physics_hook.py'
    spec = importlib.util.spec_from_file_location('test_usd_physics_hook', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, bpy


def test_generic_spring_d6_export_import(hook):
    module, bpy = hook
    stage = Usd.Stage.CreateInMemory()
    stage.DefinePrim('/BodyA', 'Xform')
    stage.DefinePrim('/BodyB', 'Xform')
    a = SimpleNamespace(name='BodyA', matrix_world=ZMatrix())
    b = SimpleNamespace(name='BodyB', matrix_world=ZMatrix())
    constraint = SimpleNamespace(type='GENERIC_SPRING', object1=a, object2=b,
        enabled=True, use_spring_y=True, spring_stiffness_y=12.0,
        spring_damping_y=3.0, use_spring_ang_z=True,
        spring_stiffness_ang_z=20.0, spring_damping_ang_z=4.0,
        use_limit_lin_x=True, limit_lin_x_lower=-0.4, limit_lin_x_upper=0.7,
        use_limit_ang_z=True, limit_ang_z_lower=-math.pi/4, limit_ang_z_upper=math.pi/2)
    obj = SimpleNamespace(name='joint', matrix_world=ZMatrix(), rigid_body_constraint=constraint)
    module._export_constraint(stage, obj, 'USD')
    prim = stage.GetPrimAtPath('/Joints/joint')
    assert prim.GetTypeName() == 'PhysicsJoint'
    assert prim.HasAPI(UsdPhysics.DriveAPI, 'transY')
    assert prim.HasAPI(UsdPhysics.DriveAPI, 'rotZ')
    assert not prim.HasAPI(UsdPhysics.DriveAPI, 'transX')
    assert UsdPhysics.DriveAPI(prim, 'transY').GetStiffnessAttr().Get() == pytest.approx(12)
    assert UsdPhysics.DriveAPI(prim, 'rotZ').GetStiffnessAttr().Get() == pytest.approx(20*math.pi/180)
    assert UsdPhysics.LimitAPI(prim, 'rotZ').GetLowAttr().Get() == pytest.approx(-45)
    data = module._read_joint(prim, 'USD')
    assert [name for name, _ in module._read_usd_physics(stage, 'USD')['joints']] == ['joint']
    assert data['axes']['transY']['damping'] == pytest.approx(3)
    assert data['axes']['rotZ']['stiffness'] == pytest.approx(20)
    assert data['axes']['rotZ']['lower'] == pytest.approx(-math.pi/4)
    assert data['body0'] == '/BodyA' and data['body1'] == '/BodyB'
    out = SimpleNamespace(type=None, enabled=None)
    target = SimpleNamespace(rigid_body_constraint=out)
    bpy.data = SimpleNamespace(objects={'joint': target, 'BodyA': a, 'BodyB': b})
    bpy.context = SimpleNamespace(scene=SimpleNamespace(),
                                  view_layer=SimpleNamespace(objects=SimpleNamespace(active=None)))
    module._apply_physics_to_blender({'scene': None, 'bodies': [], 'joints': [('joint', data)]})
    assert out.type == 'GENERIC_SPRING' and out.use_spring_y and out.use_spring_ang_z
    assert not out.use_spring_x and out.spring_stiffness_ang_z == pytest.approx(20)
    assert out.use_limit_lin_x and out.limit_lin_x_upper == pytest.approx(0.7)
    assert out.use_limit_ang_z and out.limit_ang_z_lower == pytest.approx(-math.pi/4)


def test_mujoco_only_retains_scalar_spring_mapping(hook):
    module, _ = hook
    stage = Usd.Stage.CreateInMemory()
    body = SimpleNamespace(name='body')
    constraint = SimpleNamespace(type='GENERIC_SPRING', object1=body, object2=body,
                                 spring_damping_x=2.0, spring_stiffness_x=5.0)
    obj = SimpleNamespace(name='joint', rigid_body_constraint=constraint)
    module._export_constraint(stage, obj, 'MJC')
    prim = stage.GetPrimAtPath('/Joints/joint')
    assert prim.GetTypeName() == 'PhysicsDistanceJoint'
    assert prim.GetAttribute('mjc:stiffness').Get() == pytest.approx(5)


def test_export_joint_frames_use_body_and_constraint_world_transforms(hook):
    module, bpy = hook
    stage = Usd.Stage.CreateInMemory()
    stage.DefinePrim('/BodyA', 'Xform')
    stage.DefinePrim('/BodyB', 'Xform')
    a = SimpleNamespace(name='BodyA', matrix_world=ZMatrix(math.pi/2, (1, 0, 0)))
    b = SimpleNamespace(name='BodyB', matrix_world=ZMatrix())
    constraint = SimpleNamespace(type='FIXED', object1=a, object2=b, enabled=True)
    obj = SimpleNamespace(name='joint', matrix_world=ZMatrix(math.pi, (2, 0, 0)),
                          rigid_body_constraint=constraint)
    module._export_constraint(stage, obj, 'USD')
    prim = stage.GetPrimAtPath('/Joints/joint')
    assert tuple(prim.GetAttribute('physics:localPos0').Get()) == pytest.approx((0, -1, 0), abs=1e-6)
    assert tuple(prim.GetAttribute('physics:localPos1').Get()) == pytest.approx((2, 0, 0), abs=1e-6)
    rot0 = prim.GetAttribute('physics:localRot0').Get()
    rot1 = prim.GetAttribute('physics:localRot1').Get()
    assert rot0.GetReal() == pytest.approx(math.sqrt(0.5), abs=1e-6)
    assert tuple(rot0.GetImaginary()) == pytest.approx((0, 0, math.sqrt(0.5)), abs=1e-6)
    assert rot1.GetReal() == pytest.approx(0, abs=1e-6)
    assert tuple(rot1.GetImaginary()) == pytest.approx((0, 0, 1), abs=1e-6)
    target = SimpleNamespace(rigid_body_constraint=SimpleNamespace(type=None, enabled=None))
    bpy.data = SimpleNamespace(objects={'joint': target, 'BodyA': a, 'BodyB': b})
    bpy.context = SimpleNamespace(scene=SimpleNamespace(),
                                  view_layer=SimpleNamespace(objects=SimpleNamespace(active=None)))
    module._apply_physics_to_blender({'scene': None, 'bodies': [],
                                      'joints': [('joint', module._read_joint(prim, 'USD'))]})
    np.testing.assert_allclose(target.matrix_world.matrix, obj.matrix_world.matrix, atol=1e-6)


def test_export_joint_skips_singular_body_transform(hook):
    module, _ = hook
    stage = Usd.Stage.CreateInMemory()
    body = SimpleNamespace(name='body', matrix_world=ZMatrix(matrix=np.zeros((4, 4))))
    constraint = SimpleNamespace(type='FIXED', object1=body, object2=body, enabled=True)
    obj = SimpleNamespace(name='joint', matrix_world=ZMatrix(), rigid_body_constraint=constraint)
    module._export_constraint(stage, obj, 'USD')
    assert not stage.GetPrimAtPath('/Joints/joint')


def test_collision_collection_masks_export_and_import(hook):
    module, bpy = hook
    stage = Usd.Stage.CreateInMemory()
    masks = [0, 1, 2, 3, 1]
    objects = []
    for index, mask in enumerate(masks):
        name = f'Body{index}'
        prim = stage.DefinePrim('/' + name, 'Cube')
        UsdPhysics.RigidBodyAPI.Apply(prim)
        UsdPhysics.CollisionAPI.Apply(prim)
        flags = [bool(mask & (1 << i)) for i in range(20)]
        objects.append(SimpleNamespace(name=name, rigid_body=SimpleNamespace(collision_collections=flags)))
    module._export_collision_groups(stage, objects)
    for mask in set(masks):
        group = UsdPhysics.CollisionGroup.Get(stage, f'/BlenderPhysicsCollisionGroups/mask_{mask:05x}')
        assert group
        members = {str(p) for p in group.GetCollidersCollectionAPI().GetIncludesRel().GetTargets()}
        assert members == {f'/Body{i}' for i, m in enumerate(masks) if m == mask}
        assert group.GetCollidersCollectionAPI().GetExpansionRuleAttr().Get() == 'explicitOnly'
        filtered = {str(p) for p in group.GetFilteredGroupsRel().GetTargets()}
        for other in set(masks):
            assert (f'/BlenderPhysicsCollisionGroups/mask_{other:05x}' in filtered) == (mask & other == 0)
    bodies = module._read_usd_physics(stage, 'USD')['bodies']
    assert {name: data['collision_collections'] for name, data in bodies} == {
        obj.name: obj.rigid_body.collision_collections for obj in objects}
    imported = {obj.name: SimpleNamespace(rigid_body=SimpleNamespace(), parent=None)
                for obj in objects}
    bpy.data = SimpleNamespace(objects=imported)
    bpy.context = SimpleNamespace(scene=SimpleNamespace(),
                                  view_layer=SimpleNamespace(objects=SimpleNamespace(active=None)))
    module._apply_physics_to_blender({'scene': None, 'bodies': bodies, 'joints': []})
    for obj in objects:
        assert imported[obj.name].rigid_body.collision_collections == obj.rigid_body.collision_collections


def test_mujoco_sliding_friction_maps_to_bullet(hook):
    module, _ = hook
    stage = Usd.Stage.CreateInMemory()
    body = stage.DefinePrim('/Body', 'Cube')
    UsdPhysics.RigidBodyAPI.Apply(body)
    collider = stage.DefinePrim('/Body/Collider', 'Cube')
    UsdPhysics.CollisionAPI.Apply(collider)
    collider.CreateAttribute('mjc:geomFriction', Sdf.ValueTypeNames.DoubleArray).Set(
        [0.8, 0.005, 0.0001])
    assert module._read_rigid_body(body, 'MJC')['friction'] == pytest.approx(0.8)

    collider2 = stage.DefinePrim('/Body/Collider2', 'Cube')
    UsdPhysics.CollisionAPI.Apply(collider2)
    collider2.CreateAttribute('mjc:geomFriction', Sdf.ValueTypeNames.DoubleArray).Set(
        [0.6, 0.005, 0.0001])
    assert module._read_rigid_body(body, 'MJC')['friction'] == pytest.approx(0.7)

    # In combined mode, standard USD material data wins when present; it is
    # the closest authored source for Blender's scalar friction property.
    material = UsdPhysics.MaterialAPI.Apply(body)
    material.CreateStaticFrictionAttr(0.3)
    assert module._read_rigid_body(body, 'BOTH')['friction'] == pytest.approx(0.3)

    bad_body = stage.DefinePrim('/BadBody', 'Cube')
    UsdPhysics.RigidBodyAPI.Apply(bad_body)
    bad_collider = stage.DefinePrim('/BadBody/Collider', 'Cube')
    UsdPhysics.CollisionAPI.Apply(bad_collider)
    bad_collider.CreateAttribute('mjc:geomFriction', Sdf.ValueTypeNames.DoubleArray).Set(
        [float('nan'), 0.005, 0.0001])
    assert module._read_rigid_body(bad_body, 'MJC')['friction'] == pytest.approx(0.5)


def test_collision_group_limit_warns_and_retains_masks(hook, caplog):
    module, _ = hook
    stage = Usd.Stage.CreateInMemory()
    objects = []
    for mask in range(513):
        name = f'B{mask}'
        stage.DefinePrim('/' + name, 'Cube')
        objects.append(SimpleNamespace(name=name, rigid_body=SimpleNamespace(
            collision_collections=[bool(mask & (1 << i)) for i in range(20)])))
    module._export_collision_groups(stage, objects)
    assert 'Too many distinct collision masks' in caplog.text
    assert not stage.GetPrimAtPath('/BlenderPhysicsCollisionGroups')
    assert stage.GetPrimAtPath('/B512').GetAttribute('userProperties:blender:collisionMask').Get() == 512


@pytest.mark.parametrize('timestep', [1.001 / 60, 0.003, 1 / 48])
def test_fractional_timestep_preserves_effective_rate(hook, timestep):
    module, bpy = hook
    render = SimpleNamespace(fps=24, fps_base=1.0)
    bpy.context = SimpleNamespace(scene=SimpleNamespace(
        render=render, rigidbody_world=SimpleNamespace()))
    module._apply_physics_to_blender({
        'scene': {'mjc': {'timestep': timestep}}, 'bodies': [], 'joints': []})
    assert render.fps_base / render.fps == pytest.approx(timestep)


@pytest.mark.parametrize('timestep', [0, -1, float('nan'), float('inf'), 1e-320, 1e300])
def test_invalid_or_unsupported_timestep_keeps_scene_rate(hook, timestep, caplog):
    module, bpy = hook
    render = SimpleNamespace(fps=24, fps_base=1.25)
    bpy.context = SimpleNamespace(scene=SimpleNamespace(
        render=render, rigidbody_world=SimpleNamespace()))
    module._apply_physics_to_blender({
        'scene': {'mjc': {'timestep': timestep}}, 'bodies': [], 'joints': []})
    assert (render.fps, render.fps_base) == (24, 1.25)
    assert 'timestep' in caplog.text
