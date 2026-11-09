# SPDX-License-Identifier: Apache-2.0
"""Atomic component layout (lightusd.asset.save_component)."""
import numpy as np
import pytest

import lightusd
from lightusd import asset

ROBOT = b'''<mujoco model="arm"><asset><material name="red" rgba="1 0 0 1"/></asset>
<worldbody><body name="base" pos="0 0 .1"><geom type="box" size=".1 .1 .1" material="red"/>
  <body name="link" pos="0 0 .2"><joint name="j" axis="0 1 0" range="-1 1" damping=".1"/>
    <geom type="capsule" fromto="0 0 0 0 0 .3" size=".03"/><site name="tip" pos="0 0 .3"/></body></body></worldbody>
<actuator><position joint="j" kp="10"/></actuator></mujoco>'''


def _snapshot(stage, root):
    out = {}
    for prim in stage:
        if not (prim.path == root or prim.path.startswith(root + "/")):
            continue
        rel = prim.path[len(root):]
        props = {}
        for name in prim.attributes:
            attribute = prim.attribute(name)
            try:
                value = attribute.get()
            except KeyError:
                value = None
            props[name] = (attribute.type_name, None if value is None else np.asarray(value, dtype=object).tolist(),
                           tuple(c[len(root):] if c.startswith(root) else c for c in attribute.connections))
        for name in prim.relationships:
            props['rel:' + name] = tuple(t[len(root):] if t.startswith(root) else t
                                         for t in prim.relationship(name).targets)
        out[rel] = (prim.type_name, tuple(sorted(prim.metadata('apiSchemas') or ())), props)
    return out


def test_component_layers_compose_back_to_the_source(tmp_path):
    source = lightusd.load_bytes(ROBOT, format='mjcf')
    layers = asset.save_component(source, tmp_path / 'arm', name='Arm')
    assert layers.interface.endswith('Arm.usda')
    interface = lightusd.load(layers.interface, composed=False)
    top = interface.prim_at('/Arm')
    assert top.metadata('kind') == 'component'
    composed = lightusd.load(layers.interface)
    assert composed.default_prim.path == '/Arm'
    a, b = _snapshot(source, '/World'), _snapshot(composed, '/Arm')
    assert set(a) == set(b)
    for path in a:
        assert a[path][0] == b[path][0], path
        assert a[path][1] == b[path][1], path
        assert a[path][2] == b[path][2], path
    # Physics opinions live in Physics.usda, geometry in Geometry.usda.
    physics = lightusd.load(layers.physics, composed=False)
    assert physics.prim_at('/Arm/Joints/j').type_name == 'PhysicsRevoluteJoint'
    assert physics.prim_at('/Arm/Links/link').specifier == 'over'
    geometry = lightusd.load(layers.geometry, composed=False)
    link = geometry.prim_at('/Arm/Links/link')
    assert 'physics:mass' not in link.attributes and 'xformOp:transform' in link.attributes
    materials = lightusd.load(layers.materials, composed=False)
    assert materials.prim_at('/Arm/Materials/red').type_name == 'Material'


def test_component_copies_textures(tmp_path):
    import base64
    png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==')
    (tmp_path / 'src').mkdir()
    (tmp_path / 'src' / 'grid.png').write_bytes(png)
    stage = lightusd.Stage.create()
    stage.define_prim('/World', 'Xform')
    stage.set_default_prim('World')
    shader = stage.define_prim('/World/Materials/m/tex', 'Shader')
    shader.set('inputs:file', 'grid.png', type='asset')
    shader.set('inputs:other', str(tmp_path / 'src' / 'grid.png'), type='asset')
    layers = asset.save_component(stage, tmp_path / 'out', name='Tex', asset_root=tmp_path / 'src')
    copied = tmp_path / 'out' / 'Payload' / 'Textures' / 'grid.png'
    assert copied.read_bytes() == png
    materials = lightusd.load(layers.materials, composed=False)
    tex = materials.prim_at('/Tex/Materials/m/tex')
    assert tex['inputs:file'] == tex['inputs:other'] == './Textures/grid.png'
    kept = asset.save_component(stage, tmp_path / 'kept', name='Tex', textures='keep')
    assert lightusd.load(kept.materials, composed=False).prim_at('/Tex/Materials/m/tex')['inputs:file'] == 'grid.png'


def test_declare_and_list_op_api_schemas():
    stage = lightusd.Stage.create()
    material = stage.define_prim('/M', 'Material')
    material.declare('outputs:surface', 'token')
    stage.prim_at('/M').attribute('outputs:surface').connect('/M/S.outputs:surface')
    material.declare('uniform_thing', 'float[]', uniform=True)
    stage.prim_at('/M').set_metadata('prepend apiSchemas', ('MaterialBindingAPI',))
    text = stage.export_usda()
    assert 'token outputs:surface.connect = </M/S.outputs:surface>' in text
    assert 'uniform float[] uniform_thing' in text
    assert 'prepend apiSchemas = ["MaterialBindingAPI"]' in text
    with pytest.raises(ValueError):
        stage.prim_at('/M').set_metadata('reorder apiSchemas', ('X',))
    with pytest.raises(ValueError):
        stage.prim_at('/M').declare('x', 'notatype')


def test_shared_meshes_are_written_once_to_the_geometry_library(tmp_path):
    import struct
    tri = struct.pack("<12fH", 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0)
    (tmp_path / "wheel.stl").write_bytes(b"\0" * 80 + struct.pack("<I", 1) + tri)
    wheels = "".join(f'<body name="w{i}" pos="{i} 0 0"><geom type="mesh" mesh="wheel" rgba="{i % 2} 0 0 1"/></body>'
                     for i in range(4))
    (tmp_path / "car.xml").write_text(f'<mujoco><asset><mesh file="wheel.stl"/></asset><worldbody>'
                                      f'<body name="base"><freejoint/>{wheels}</body></worldbody></mujoco>')
    source = lightusd.import_mjcf(tmp_path / "car.xml")
    meshes = [p for p in source if p.type_name == "Mesh"]
    assert len(meshes) >= 4
    layers = asset.save_component(source, tmp_path / "car", name="Car")
    library = lightusd.load(layers.geometry_library, composed=False)
    assert [p.name for p in library if p.type_name == "Mesh"] == [meshes[0].name]
    geometry = lightusd.load(layers.geometry, composed=False)
    for mesh in meshes:
        local = geometry.prim_at(mesh.path.replace("/World", "/Car", 1))
        assert "points" not in local.attributes
        assert ("primvars:displayColor" in local.attributes) == ("primvars:displayColor" in mesh.attributes)
    composed = lightusd.load(layers.interface)
    assert _snapshot(source, "/World") == _snapshot(composed, "/Car")
    flat = asset.save_component(source, tmp_path / "flat", name="Car", geometry_library=False)
    assert flat.geometry_library is None
