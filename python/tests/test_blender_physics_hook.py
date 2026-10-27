# SPDX-License-Identifier: Apache-2.0
"""Exercise the addon's schema mapping with OpenUSD and a minimal bpy stub.

These tests do not replace a Blender integration run.
"""
import importlib.util
import math
import pathlib
import sys
from types import SimpleNamespace

import pytest

pxr = pytest.importorskip('pxr')
from pxr import Gf, Usd, UsdPhysics


@pytest.fixture
def hook(monkeypatch):
    bpy = SimpleNamespace(
        types=SimpleNamespace(AddonPreferences=object, USDHook=object),
        props=SimpleNamespace(EnumProperty=lambda **kwargs: None),
    )
    monkeypatch.setitem(sys.modules, 'bpy', bpy)
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
    a = SimpleNamespace(name='BodyA', location=Gf.Vec3d(0, 0, 0))
    b = SimpleNamespace(name='BodyB', location=Gf.Vec3d(0, 0, 0))
    constraint = SimpleNamespace(type='GENERIC_SPRING', object1=a, object2=b,
        enabled=True, use_spring_y=True, spring_stiffness_y=12.0,
        spring_damping_y=3.0, use_spring_ang_z=True,
        spring_stiffness_ang_z=20.0, spring_damping_ang_z=4.0,
        use_limit_lin_x=True, limit_lin_x_lower=-0.4, limit_lin_x_upper=0.7,
        use_limit_ang_z=True, limit_ang_z_lower=-math.pi/4, limit_ang_z_upper=math.pi/2)
    obj = SimpleNamespace(name='joint', location=Gf.Vec3d(0, 0, 0), rigid_body_constraint=constraint)
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
