# SPDX-License-Identifier: Apache-2.0
import base64
import math
import pathlib
import zipfile

import numpy as np
import pytest
import lightusd
from lightusd import physics, tydra

OBJ = b'''o panel
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 -1
f 1/1/1 2/2/1 3/3/1 4/4/1
'''
URDF = '''<robot name="test"><link name="base"/><link name="arm">
<inertial><origin xyz=".1 .2 .3" rpy="0 0 1.5707963267948966"/>
<mass value="2"/><inertia ixx="1" iyy="2" izz="3" ixy=".1" ixz="0" iyz="0"/></inertial>
<collision><geometry><box size="1 2 3"/></geometry></collision></link>
<joint name="hinge" type="revolute"><parent link="base"/><child link="arm"/>
<origin xyz="1 2 3" rpy="0 0 1.5707963267948966"/><axis xyz="0 1 -1"/>
<limit lower="-1" upper="2"/><dynamics damping=".25"/></joint></robot>'''


def mesh(stage):
    return next(p for p in stage if p.type_name == "Mesh")


def test_obj_polygon_normals_uv_and_no_physics():
    stage = lightusd.load_bytes(OBJ, format="obj")
    m = mesh(stage)
    assert list(m["faceVertexCounts"]) == [4]
    np.testing.assert_allclose(np.asarray(m["normals"]), [[0, 0, -1]] * 4)
    np.testing.assert_allclose(np.asarray(m["primvars:st"]), [[0, 0], [1, 0], [1, 1], [0, 1]])
    assert not physics.inspect(stage).scenes
    # OBJ has no units: the current profile states meters explicitly; the
    # legacy profile leaves USD's fallback.
    assert "metersPerUnit = 1" in stage.export_usda()
    legacy = lightusd._import(data=OBJ, format="obj", profile="legacy")
    assert "metersPerUnit" not in legacy.export_usda()


@pytest.mark.parametrize("extension", ["obj", "OBJ"])
def test_obj_direct_file(tmp_path, extension):
    path = tmp_path / f"mesh.{extension}"
    path.write_bytes(OBJ)
    assert mesh(lightusd.load(path)).type_name == "Mesh"
    assert mesh(lightusd.import_obj(path)).type_name == "Mesh"
    assert not lightusd.is_usd(path)


def test_obj_groups_and_negative_indices():
    stage = lightusd.load_bytes(OBJ + b'o second\nf -4/-4/1 -2/-2/1 -1/-1/1\n', format="obj")
    assert len(stage.prims_of_type("Mesh")) == 2


def test_obj_incomplete_uv_warning():
    stage = lightusd.load_bytes(OBJ.replace(b'4/4/1', b'4//1'), format="obj")
    assert "primvars:st" not in mesh(stage)
    assert "incomplete UV" in stage.warnings()


@pytest.mark.parametrize("bad", [b'f 99 98 97\n', b'v nan 0 0\nf 1 1 1\n', b'v 0 0 0\nf 0 0 0\n'])
def test_obj_invalid_rejected(bad):
    with pytest.raises(lightusd.UsdError):
        lightusd.load_bytes(bad, format="obj")


def test_obj_smoothing_and_concave_robot(tmp_path):
    path = tmp_path / "mesh.obj"
    path.write_text('v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\ns 1\nf 1 2 3 4 5\n')
    robot = tmp_path / "robot.urdf"
    robot.write_text('<robot name="r"><link name="b"><visual><geometry><mesh filename="mesh.obj"/></geometry></visual></link></robot>')
    m = mesh(lightusd.import_urdf(robot))
    assert list(m["faceVertexCounts"]) == [3, 3, 3]
    points = np.asarray(m["points"])
    indices = np.asarray(m["faceVertexIndices"]).reshape(-1, 3)
    area = sum(np.linalg.norm(np.cross(points[t[1]]-points[t[0]], points[t[2]]-points[t[0]]))/2 for t in indices)
    assert area == pytest.approx(3)


def test_mtl_maps_subsets_and_usdz(tmp_path):
    (tmp_path / "mats").mkdir()
    png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j1ZkAAAAASUVORK5CYII=')
    (tmp_path / "mats" / "color.png").write_bytes(png)
    (tmp_path / "mats" / "test.mtl").write_text('newmtl red\nKd .8 .1 .2\nKe .2 .3 .4\nNi 1.4\nPr .3\nPm .2\nmap_Kd -s 2 3 1 -o .1 .2 0 -clamp on color.png\nnorm color.png\n')
    path = tmp_path / "mesh.obj"
    path.write_bytes(b'mtllib mats/test.mtl\nusemtl red\n' + OBJ)
    stage = lightusd.load(path)
    assert len(stage.prims_of_type("Material")) == 1
    subset = stage.prims_of_type("GeomSubset")[0]
    assert subset.relationship("material:binding").targets == ("/World/Materials/red",)
    surface = stage.prim_at("/World/Materials/red/PreviewSurface")
    assert surface["inputs:ior"] == pytest.approx(1.4)
    assert surface.attribute("inputs:diffuseColor").connections
    assert surface.attribute("inputs:normal").type_name == "normal3f"
    assert surface.attribute("inputs:normal").connections
    texture = next(p for p in stage if p.type_name == "Shader" and p.get("info:id") == "UsdUVTexture")
    assert texture["inputs:file"] == (tmp_path / "mats" / "color.png").as_posix()
    assert texture["inputs:wrapS"] == "clamp"
    for save in (stage.save, stage.save_usdz):
        output = tmp_path / "out.usdz"
        save(str(output))
        with zipfile.ZipFile(output) as archive:
            assert any(archive.read(n) == png for n in archive.namelist() if n.endswith('.png'))
        loaded = lightusd.load(output)
        assert '.png' in loaded.export_usda()
    assert texture["inputs:file"] == (tmp_path / "mats" / "color.png").as_posix()


def test_missing_mtl_warns(tmp_path):
    path = tmp_path / "mesh.obj"
    path.write_bytes(b'mtllib absent.mtl\n' + OBJ)
    assert "MTL" in lightusd.load(path).warnings()


def test_urdf_frames_inertia_and_authoring(tmp_path):
    path = tmp_path / "robot.urdf"
    path.write_text(URDF)
    stage = lightusd.load(path)
    data = physics.inspect(stage)
    assert len(data.scenes) == 1
    assert len(data.colliders) == 1
    arm = stage.prim_at("/World/Links/arm")
    np.testing.assert_allclose(np.array(arm.local_transform())[3, :3], [1, 2, 3])
    inertia = np.asarray(arm["physics:diagonalInertia"])
    assert sorted(inertia) == pytest.approx(np.linalg.eigvalsh([[1, .1, 0], [.1, 2, 0], [0, 0, 3]]))
    joint = next(j for j in data.joints if j.prim_path.endswith('/hinge'))
    assert joint.attributes["physics:upperLimit"] == pytest.approx(math.degrees(2))
    assert joint.relationships["physics:body1"] == ('/World/Links/arm',)
    assert joint.attributes["physics:localRot1"] != (1, 0, 0, 0)
    actuator = physics.define_prim(stage, '/World/Drive', 'NewtonActuator', api_schemas=['NewtonPDControlAPI'],
        attributes={'newton:kp': 10}, relationships={'newton:targets': ['/World/Joints/hinge']})
    assert actuator['newton:kp'] == 10
    physics.apply_schema(actuator, 'NewtonMaxEffortClampingAPI', attributes={'newton:maxEffort': 5})
    assert 'NewtonPDControlAPI' in actuator.metadata('apiSchemas')
    assert len(physics.inspect(stage).actuators) == 1
    snapshot = physics.inspect(stage)
    stage.close()
    assert snapshot.actuators[0].attributes['newton:kp'] == 10


def test_urdf_packages_and_missing(tmp_path):
    assets = tmp_path / 'assets'
    assets.mkdir()
    (assets / 'mesh.obj').write_bytes(OBJ)
    path = tmp_path / 'robot.urdf'
    path.write_text('<robot name="r"><link name="b"><visual><geometry><mesh filename="package://part/mesh.obj"/></geometry></visual></link></robot>')
    assert mesh(lightusd.import_urdf(path, packages={'part': assets})).type_name == 'Mesh'
    with pytest.raises(lightusd.UsdError): lightusd.import_urdf(path)
    stage = lightusd.import_urdf(path, allow_missing=True)
    assert not stage.prims_of_type('Mesh')
    assert 'package' in stage.warnings()


def test_mjcf_root_joint_gravity_and_roundtrip(tmp_path):
    text = b'''<mujoco model="r"><option gravity="0 0 -3" timestep=".005"/><worldbody>
    <body name="base" pos="1 2 3"><joint name="hinge" axis="0 1 -1" pos=".1 .2 .3"/>
    <geom type="box" size=".1 .2 .3" group="3"/><body name="tip" pos="0 0 1"><freejoint/>
    <geom type="sphere" size=".1"/></body></body></worldbody>
    <actuator><position name="drive" joint="hinge" kp="20" kv="2" forcerange="-3 3"/></actuator></mujoco>'''
    stage = lightusd.load_bytes(text, format='mjcf')
    assert stage.prim_at('/World/Joints/hinge').type_name == 'PhysicsRevoluteJoint'
    assert stage.prim_at('/World/PhysicsScene')['physics:gravityMagnitude'] == 3
    assert stage.prim_at('/World/PhysicsScene')['newton:timeStepsPerSecond'] == 200
    # A nested <freejoint> body is unjointed and roots its own articulation;
    # only the legacy profile also records the historical mjc:freeJoint flag.
    tip = stage.prim_at('/World/Links/tip')
    assert 'PhysicsArticulationRootAPI' in tip.metadata('apiSchemas')
    assert not any(j.relationships.get('physics:body1') == ('/World/Links/tip',)
                   for j in physics.inspect(stage).joints)
    legacy = lightusd._import(data=text, format='mjcf', profile='legacy')
    assert legacy.prim_at('/World/Links/tip')['mjc:freeJoint'] is True
    # <position kp kv> expands like MuJoCo's compiler: gain kp, affine bias
    # (0, -kp, -kv); Newton maps this shape back to a joint-target drive.
    drive = stage.prim_at('/World/MjcActuators/drive')
    assert tuple(drive['mjc:gainPrm'])[:1] == (20,) and len(drive['mjc:gainPrm']) == 10
    assert tuple(drive['mjc:biasPrm'])[:3] == (0, -20, -2)
    assert drive['mjc:biasType'] == 'affine' and drive['mjc:forceRange:max'] == 3
    assert tydra.to_render_scene(stage).physics['joints']
    for ext in ('usda', 'usdc', 'usdz'):
        path = tmp_path / ('out.' + ext)
        stage.save(str(path))
        assert physics.inspect(lightusd.load(path)).actuators[0].attributes['mjc:gainPrm'][0] == 20
    legacy = lightusd._import(data=text, format='mjcf', profile='legacy')
    assert legacy.prim_at('/World/Actuators/drive')['newton:kp'] == 20


def test_xml_detection_includes_and_anchor(tmp_path):
    (tmp_path / 'sub').mkdir()
    (tmp_path / 'sub' / 'mesh.obj').write_bytes(OBJ)
    (tmp_path / 'sub' / 'child.xml').write_text('<mujoco><asset><mesh name="m" file="mesh.obj"/></asset><worldbody><body name="b"><geom type="mesh" mesh="m"/></body></worldbody></mujoco>')
    path = tmp_path / 'main.xml'
    path.write_text('<mujoco><include file="sub/child.xml"/></mujoco>')
    assert mesh(lightusd.load(path)).type_name == 'Mesh'
    (tmp_path / 'sub' / 'child.xml').write_text('<mujoco><include file="../main.xml"/></mujoco>')
    with pytest.raises(lightusd.UsdError, match='[Rr]ecurs'): lightusd.load(path)


@pytest.mark.parametrize('text', [b'<robot>', b'<unknown/>', b'<!DOCTYPE robot><robot/>',
    b'<robot><link name="b"/><joint name="j" type="fixed"><parent link="b"/><child link="b"/></joint></robot>'])
def test_xml_invalid(text):
    with pytest.raises(lightusd.UsdError): lightusd.load_bytes(text, format='urdf')


def test_import_budget_and_resolver(tmp_path):
    with pytest.raises(lightusd.UsdError): lightusd.load_bytes(OBJ, format='obj', max_memory=16)
    resolver = lightusd.AssetResolver()
    resolver.register_memory('virtual.obj', OBJ)
    stage = lightusd.import_obj('virtual.obj', resolver=resolver)
    assert mesh(stage).type_name == 'Mesh'
    with pytest.raises(ValueError): lightusd.load_bytes(OBJ, format='obj', up_axis='X')
    path = tmp_path / 'robot.xml'
    path.write_text('<robot name="r"><link name="b"><inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link></robot>')
    assert physics.inspect(lightusd.load(path)).rigid_bodies


def test_obj_smoothing_groups():
    text = b'v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\ns 1\nf 1 2 3\nf 1 3 4\n'
    smooth = mesh(lightusd.load_bytes(text, format='obj'))
    np.testing.assert_allclose(np.asarray(smooth['normals'])[0], [2**-.5, 0, 2**-.5], atol=1e-6)
    flat = mesh(lightusd.load_bytes(text.replace(b's 1', b's off'), format='obj'))
    assert len(flat['points']) == 6


def test_no_unanchored_asset_reads_and_schema_validation():
    stage = lightusd.load_bytes(b'mtllib unknown.mtl\n' + OBJ, format='obj')
    assert 'source anchor' in stage.warnings()
    prim = stage.prim_at('/World')
    with pytest.raises(ValueError): physics.apply_schema(prim, 'NewtonTypoAPI')
    with pytest.raises(ValueError): physics.apply_schema(prim, 'NewtonSceneAPI', attributes={'newton:unknown': 1})
    physics.apply_schema(prim, 'NewtonSceneAPI', attributes={'newton:unknown': ('double[]', [1, 2])})
    assert tuple(prim['newton:unknown']) == (1, 2)


def test_resolver_callback_exception_propagation():
    resolver = lightusd.AssetResolver()
    def fail(*args):
        raise RuntimeError('import resolver failure')
    resolver.register_scheme('testimport', fail)
    with pytest.raises(RuntimeError, match='import resolver failure'):
        lightusd.import_obj('testimport:mesh.obj', resolver=resolver)


def test_mjcf_motor_bounds_are_not_constant_commands():
    text = b'''<mujoco><worldbody><body name="b"><joint name="j"/>
    <geom type="sphere" size=".1"/></body></worldbody>
    <actuator><motor name="drive" joint="j" gear="2" ctrlrange="-1 1"/></actuator></mujoco>'''
    actuator = lightusd.load_bytes(text, format='mjcf').prim_at('/World/MjcActuators/drive')
    # A motor is a unit-gain force actuator: no PD gains, no bias.
    assert tuple(actuator['mjc:gainPrm'])[:2] == (1, 0)
    assert actuator['mjc:biasType'] == 'none' and actuator['mjc:gainType'] == 'fixed'
    assert tuple(actuator['mjc:gear']) == (2, 0, 0, 0, 0, 0)
    assert (actuator['mjc:ctrlRange:min'], actuator['mjc:ctrlRange:max']) == (-1, 1)
    legacy = lightusd._import(data=text, format='mjcf', profile='legacy').prim_at('/World/Actuators/drive')
    assert 'newton:constEffort' not in legacy
    assert tuple(legacy['mjc:gear']) == (2,)
    assert tuple(legacy['mjc:ctrlrange']) == (-1, 1)


def test_reserved_world_name_and_filtered_pairs():
    stage = lightusd.load_bytes(b'<robot><link name="__world"><inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link></robot>', format='urdf')
    assert stage.prim_at('/World/Links/__world')['physics:rigidBodyEnabled'] is True
    data = lightusd.loads('''#usda 1.0
    def Xform "a" (prepend apiSchemas = ["PhysicsFilteredPairsAPI"]) {
        rel physics:filteredPairs = </b>
    }
    def Xform "b" {}
    ''')
    assert tydra.to_render_scene(data).physics['filtered_pairs'][0]['targets'] == ('/b',)


def test_visible_mjcf_geom_also_collides():
    stage = lightusd.load_bytes(b'''<mujoco><worldbody><body name="b"><freejoint/>
        <geom name="ball" type="sphere" size=".2" friction=".6 .01 .02" contype="2" conaffinity="4"/>
        </body></worldbody></mujoco>''', format='mjcf')
    data = physics.inspect(stage)
    assert len(data.colliders) == 1
    collider = data.colliders[0]
    assert collider.type_name == 'Sphere'
    assert collider.attributes['mjc:contype'] == 2
    assert collider.attributes['mjc:conaffinity'] == 4
    # Friction is a bound physics material (static == dynamic == mu[0]).
    material_path = collider.relationships['material:binding:physics'][0]
    material = stage.prim_at(material_path)
    assert 'PhysicsMaterialAPI' in material.metadata('apiSchemas')
    assert material['physics:staticFriction'] == pytest.approx(.6)
    assert material['physics:dynamicFriction'] == pytest.approx(.6)
    assert material['newton:torsionalFriction'] == pytest.approx(.01)
    assert material['newton:rollingFriction'] == pytest.approx(.02)
    assert 'mjc:geomFriction' not in collider.attributes
    legacy = physics.inspect(lightusd._import(data=b'''<mujoco><worldbody><body name="b"><freejoint/>
        <geom name="ball" type="sphere" size=".2" friction=".6 .01 .02" contype="2" conaffinity="4"/>
        </body></worldbody></mujoco>''', format='mjcf', profile='legacy')).colliders[0]
    assert legacy.attributes['mjc:geomFriction'] == pytest.approx((.6, .01, .02))
    assert legacy.attributes['mjc:geomContype'] == 2


def test_mjcf_actuator_default_class():
    stage = lightusd.load_bytes(b'''<mujoco><default><default class="drive">
      <general gainprm="4 5 6" biasprm="0 -4 -2" forcerange="-3 3"/>
      </default></default><worldbody><body name="b"><joint name="j"/>
      <geom type="sphere" size=".1"/></body></worldbody>
      <actuator><general name="motor" class="drive" joint="j"/></actuator></mujoco>''', format='mjcf')
    record = physics.inspect(stage).actuators[0]
    assert record.attributes['mjc:gainPrm'][:3] == (4, 5, 6)
    assert record.attributes['mjc:biasPrm'][:3] == (0, -4, -2)


def test_mesh_subsets_survive_layer_growth(tmp_path):
    (tmp_path / 'mesh.obj').write_bytes(b'mtllib mats.mtl\n' + OBJ.replace(b'f 1/', b'usemtl red\nf 1/'))
    (tmp_path / 'mats.mtl').write_text('newmtl red\nKd 1 0 0\n')
    geoms = ''.join(f'<visual name="v{i}"><geometry><mesh filename="mesh.obj"/></geometry></visual>' for i in range(90))
    path = tmp_path / 'robot.urdf'
    path.write_text('<robot><link name="base">' + geoms + '</link></robot>')
    stage = lightusd.import_urdf(path)
    assert len(stage.prims_of_type('Mesh')) == 90
    for p in stage.prims_of_type('Mesh'):
        assert sum(p['faceVertexCounts']) == len(p['faceVertexIndices'])


def test_mjcf_compiler_directory_from_include(tmp_path):
    (tmp_path / 'meshes').mkdir()
    (tmp_path / 'meshes' / 'mesh.obj').write_bytes(OBJ)
    (tmp_path / 'robot.xml').write_text('<mujoco><compiler meshdir="meshes"/><worldbody><body name="b"><geom type="sphere" size=".1"/></body></worldbody></mujoco>')
    path = tmp_path / 'scene.xml'
    path.write_text('<mujoco><include file="robot.xml"/><asset><mesh name="m" file="mesh.obj"/></asset><worldbody><geom mesh="m" type="mesh"/></worldbody></mujoco>')
    assert mesh(lightusd.import_mjcf(path)).type_name == 'Mesh'


def test_closed_tendon_route(tmp_path):
    stage = lightusd.load_bytes(b'''<mujoco><worldbody><body name="b">
        <site name="a"/><site name="b" pos="1 0 0"/><geom type="sphere" size=".1"/>
        </body></worldbody><tendon><spatial name="loop"><site site="a"/>
        <site site="b"/><site site="a"/></spatial></tendon></mujoco>''', format='mjcf')
    tendon = stage.prims_of_type('MjcTendon')[0]
    assert len(tendon.relationship('mjc:path').targets) == 2
    assert tuple(tendon['mjc:path:indices']) == (0, 1, 0)
    for ext in ('usda', 'usdc', 'usdz'):
        path = tmp_path / ('loop.' + ext); stage.save(str(path))
        assert tuple(lightusd.load(path).prims_of_type('MjcTendon')[0]['mjc:path:indices']) == (0, 1, 0)


def test_robot_mesh_preserves_skin(tmp_path):
    (tmp_path / 'mesh.obj').write_bytes(OBJ + b'vw 0 0 1\nvw 1 1 1\nvw 2 2 1\nvw 3 3 1\n')
    path = tmp_path / 'robot.urdf'
    path.write_text('<robot><link name="b"><visual><geometry><mesh filename="mesh.obj"/></geometry></visual></link></robot>')
    m = mesh(lightusd.import_urdf(path))
    assert 'SkelBindingAPI' in m.metadata('apiSchemas')
    assert m.attribute('primvars:skel:jointWeights').metadata('elementSize') == 1


SITES_MJCF = b'''<mujoco><worldbody><site name="fixed" pos="1 2 3"/>
  <body name="b" pos="1 0 0" euler="0 0 90"><site name="tip" pos="0 0 1"/>
  <site pos="2 0 0"/><geom type="sphere" size=".1"/></body>
  </worldbody></mujoco>'''


def test_worldbody_and_unnamed_sites_legacy():
    stage = lightusd._import(data=SITES_MJCF, format='mjcf', profile='legacy')
    sites = physics.inspect(stage).sites
    assert len(sites) == 3
    np.testing.assert_allclose(np.asarray(stage.prim_at('/World/Sites/fixed').local_transform())[3, :3], [1, 2, 3])


def test_sites_are_rigid_body_descendants():
    # Newton only loads sites below rigid bodies; world positions must survive.
    stage = lightusd.load_bytes(SITES_MJCF, format='mjcf')
    sites = physics.inspect(stage).sites
    assert len(sites) == 3
    with pytest.raises(KeyError):
        stage.prim_at('/World/Sites')
    fixed = stage.prim_at('/World/Links/__world/fixed')
    tip = stage.prim_at('/World/Links/b/tip')
    assert 'NewtonSiteAPI' in fixed.metadata('apiSchemas')
    np.testing.assert_allclose(np.asarray(fixed.world_transform())[3, :3], [1, 2, 3], atol=1e-6)
    np.testing.assert_allclose(np.asarray(tip.world_transform())[3, :3], [1, 0, 1], atol=1e-6)
    np.testing.assert_allclose(np.asarray(tip.local_transform())[3, :3], [0, 0, 1], atol=1e-6)
    unnamed = [s for s in sites if s.prim_path.startswith('/World/Links/b/') and not s.prim_path.endswith('tip')]
    assert len(unnamed) == 1
    np.testing.assert_allclose(np.asarray(stage.prim_at(unnamed[0].prim_path).world_transform())[3, :3], [1, 2, 0], atol=1e-6)


def test_mjcf_merged_compiler_and_option(tmp_path):
    (tmp_path / 'robot.xml').write_text('<mujoco><compiler angle="radian"/><option timestep=".004"/><worldbody><body name="b" euler="0 0 1.5707963267948966"><geom type="sphere" size=".1"/></body></worldbody></mujoco>')
    path = tmp_path / 'scene.xml'
    path.write_text('<mujoco><compiler meshdir="assets"/><option gravity="0 0 -3"/><include file="robot.xml"/></mujoco>')
    stage = lightusd.import_mjcf(path)
    np.testing.assert_allclose(np.asarray(stage.prim_at('/World/Links/b').local_transform())[:3, :3], [[0,1,0],[-1,0,0],[0,0,1]], atol=1e-6)
    scene = stage.prim_at('/World/PhysicsScene')
    assert scene['physics:gravityMagnitude'] == 3
    assert scene['newton:timeStepsPerSecond'] == 250


@pytest.mark.parametrize('child_angle,parent_angle,lo,hi', [('radian','degree',-.5,.75), ('degree','radian',-30,60)])
def test_attach_joint_angle_units(tmp_path, child_angle, parent_angle, lo, hi):
    (tmp_path / 'child.xml').write_text(f'''<mujoco><compiler angle="{child_angle}"/>
      <default><default class="slider"><joint type="slide" range="-.2 .4"/></default></default>
      <worldbody><body name="root"><body name="tip"><joint name="hinge" range="{lo} {hi}"/>
      <joint name="slide" class="slider"/><geom type="sphere" size=".1"/></body></body></worldbody></mujoco>''')
    path = tmp_path / 'parent.xml'
    path.write_text(f'''<mujoco><compiler angle="{parent_angle}"/>
      <asset><model name="child" file="child.xml"/></asset><worldbody>
      <body name="base"><attach model="child" body="root" prefix="p_"/></body></worldbody></mujoco>''')
    stage = lightusd.import_mjcf(path)
    hinge = stage.prim_at('/World/Joints/p_hinge')
    scale = 180 / math.pi if child_angle == 'radian' else 1
    assert hinge['physics:lowerLimit'] == pytest.approx(lo * scale)
    assert hinge['physics:upperLimit'] == pytest.approx(hi * scale)
    slide = stage.prim_at('/World/Joints/p_slide')
    assert slide['physics:lowerLimit'] == pytest.approx(-.2)
    assert slide['physics:upperLimit'] == pytest.approx(.4)


def test_mjcf_repeated_model_sections():
    stage = lightusd.load_bytes(b'''<mujoco><worldbody><body name="b"><joint name="a"/>
      <joint name="b"/><geom type="sphere" size=".1"/></body></worldbody>
      <tendon><fixed name="t0"><joint joint="a" coef="1"/></fixed></tendon>
      <tendon><fixed name="t1"><joint joint="b" coef="1"/></fixed></tendon>
      <actuator><motor name="a0" joint="a"/></actuator><actuator><motor name="a1" joint="b"/></actuator>
      <equality><joint name="eq0" joint1="a" joint2="b"/></equality>
      <equality><joint name="eq1" joint1="b" joint2="a"/></equality></mujoco>''', format='mjcf')
    data = physics.inspect(stage)
    assert len(data.actuators) == len(data.tendons) == len(data.equalities) == 2


def test_urdf_fixed_axis_rpy():
    roll, pitch, yaw = .4, -.3, .8
    c, s = math.cos, math.sin
    rx = np.array([[1,0,0], [0,c(roll),-s(roll)], [0,s(roll),c(roll)]])
    ry = np.array([[c(pitch),0,s(pitch)], [0,1,0], [-s(pitch),0,c(pitch)]])
    rz = np.array([[c(yaw),-s(yaw),0], [s(yaw),c(yaw),0], [0,0,1]])
    rotation = rz @ ry @ rx
    stage = lightusd.load_bytes(b'''<robot><link name="base"/><link name="arm"/><link name="tip"/>
      <joint name="j" type="fixed"><parent link="base"/><child link="arm"/>
        <origin xyz=".1 .2 .3" rpy=".4 -.3 .8"/></joint>
      <joint name="tip_joint" type="fixed"><parent link="arm"/><child link="tip"/>
        <origin xyz="1 2 3"/></joint></robot>''', format='urdf')
    arm = np.asarray(stage.prim_at('/World/Links/arm').local_transform())
    tip = np.asarray(stage.prim_at('/World/Links/tip').local_transform())
    np.testing.assert_allclose(arm[:3, :3].T, rotation, atol=1e-6)
    np.testing.assert_allclose(tip[3, :3], rotation @ [1,2,3] + [.1,.2,.3], atol=1e-6)


@pytest.mark.parametrize('xml,gain,bias,dyn,extra', [
    ('<velocity joint="j"/>', (1,), (0, 0, -1), 'none', {}),
    ('<velocity joint="j" kv="3"/>', (3,), (0, 0, -3), 'none', {}),
    ('<position joint="j"/>', (1,), (0, -1, 0), 'none', {}),
    ('<position joint="j" kp="5" dampratio=".7" timeconst=".1"/>', (5,), (0, -5, .7), 'filterexact', {}),
    ('<intvelocity joint="j" kp="2" actrange="-1 1"/>', (2,), (0, -2, 0), 'integrator',
     {'mjc:actLimited': 'true', 'mjc:actRange:max': 1}),
    ('<damper joint="j" kv="4" ctrlrange="0 1"/>', (0, 0, -4), (), 'none', {'mjc:gainType': 'affine'}),
    ('<cylinder joint="j" diameter="2" timeconst=".5"/>', (3.141592653589793,), (0, 0, 0), 'filter',
     {'mjc:dynPrm': (.5, 0, 0, 0, 0, 0, 0, 0, 0, 0)}),
])
def test_mjcf_actuator_shortcut_expansion(xml, gain, bias, dyn, extra):
    stage = lightusd.load_bytes(('<mujoco><worldbody><body name="b"><joint name="j"/>'
        '<geom type="sphere" size=".1"/></body></worldbody><actuator>' + xml +
        '</actuator></mujoco>').encode(), format='mjcf')
    act = physics.inspect(stage).actuators[0].attributes
    assert act['mjc:gainPrm'][:len(gain)] == pytest.approx(gain)
    assert act['mjc:biasPrm'][:len(bias)] == pytest.approx(bias)
    assert act['mjc:dynType'] == dyn
    for key, value in extra.items():
        assert act[key] == (pytest.approx(value) if not isinstance(value, str) else value)


def test_mjcf_unsupported_actuator_gain_type_policy():
    xml = (b'<mujoco><worldbody><body name="b"><joint name="j"/><geom type="sphere" size=".1"/>'
           b'</body></worldbody><actuator><general joint="j" gaintype="dcmotor"/></actuator></mujoco>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    assert not physics.inspect(stage).actuators
    assert 'MjcPhysics token' in stage.warnings()
    with pytest.raises(Exception):
        lightusd._import(data=xml, format='mjcf', unsupported_policy='error')



URDF_PLANAR = b'''<robot name="r"><link name="base"/><link name="slider">
  <inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial>
  <visual><geometry><mesh filename="missing_geometry_type.xyz"/></geometry></visual>
  <collision><geometry/></collision></link>
  <link name="tip"><inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link>
  <joint name="plane" type="planar"><parent link="base"/><child link="slider"/><axis xyz="0 0 1"/></joint>
  <joint name="weld" type="fixed"><parent link="slider"/><child link="tip"/><axis xyz="0 0 0"/></joint>
</robot>'''


def test_urdf_planar_fixed_zero_axis_and_missing_geometry(tmp_path):
    (tmp_path / "missing_geometry_type.xyz").write_bytes(b"")
    path = tmp_path / "r.urdf"
    path.write_bytes(URDF_PLANAR.replace(b'<mesh filename="missing_geometry_type.xyz"/>', b''))
    stage = lightusd.import_urdf(path)
    plane = stage.prim_at('/World/Joints/plane')
    assert plane.type_name == 'PhysicsJoint'
    apis = set(plane.metadata('apiSchemas'))
    assert {'PhysicsLimitAPI:transX', 'PhysicsLimitAPI:rotY', 'PhysicsLimitAPI:rotZ'} <= apis
    assert plane['limit:rotY:physics:low'] > plane['limit:rotY:physics:high']
    assert stage.prim_at('/World/Joints/weld').type_name == 'PhysicsFixedJoint'
    assert 'no supported geometry' in stage.warnings()
    with pytest.raises(Exception):
        lightusd.import_urdf(path, unsupported_policy='error')
    with pytest.raises(Exception):
        lightusd.import_urdf(path, profile='legacy')


def test_ascii_stl(tmp_path):
    (tmp_path / "tri.stl").write_text("solid t\n facet normal 0 0 1\n  outer loop\n"
        "   vertex 0 0 0\n   vertex 1 0 0\n   vertex 0 1 0\n  endloop\n endfacet\n"
        " facet normal 0 0 1\n  outer loop\n   vertex 1 0 0\n   vertex 1 1 0\n   vertex 0 1 0\n"
        "  endloop\n endfacet\nendsolid t\n")
    (tmp_path / "r.urdf").write_text('<robot name="r"><link name="l"><visual><geometry>'
        '<mesh filename="tri.stl"/></geometry></visual></link></robot>')
    stage = lightusd.import_urdf(tmp_path / "r.urdf")
    m = mesh(stage)
    assert len(m["points"]) == 4 and len(m["faceVertexIndices"]) == 6



def test_binary_stl_welds_shared_vertices_in_first_use_order(tmp_path):
    import struct
    tris = [((0, 0, 0), (1, 0, 0), (0, 1, 0)), ((1, 0, 0), (1, 1, 0), (0, 1, 0)),
            ((0, 0, 0), (0, 1, 0), (-0.0, 0, 1))]  # -0.0 is a distinct bit pattern
    body = b"".join(struct.pack("<12fH", 0, 0, 1, *(c for v in t for c in v), 0) for t in tris)
    (tmp_path / "quad.stl").write_bytes(b"\0" * 80 + struct.pack("<I", len(tris)) + body)
    (tmp_path / "r.urdf").write_text('<robot name="r"><link name="l"><visual><geometry>'
        '<mesh filename="quad.stl"/></geometry></visual></link></robot>')
    m = mesh(lightusd.import_urdf(tmp_path / "r.urdf"))
    assert len(m["points"]) == 5
    assert list(m["faceVertexIndices"]) == [0, 1, 2, 1, 3, 2, 0, 2, 4]

DAE = '''<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
<asset><unit meter="0.01"/><up_axis>Y_UP</up_axis></asset>
<library_effects><effect id="red-fx"><profile_COMMON><technique sid="t"><phong>
  <diffuse><color>1 0 0 1</color></diffuse></phong></technique></profile_COMMON></effect></library_effects>
<library_materials><material id="red" name="Red"><instance_effect url="#red-fx"/></material></library_materials>
<library_geometries><geometry id="quad"><mesh>
  <source id="p"><float_array id="pa" count="12">0 0 0 100 0 0 100 100 0 0 100 0</float_array>
    <technique_common><accessor source="#pa" count="4" stride="3"/></technique_common></source>
  <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
  <polylist material="sym" count="1"><input semantic="VERTEX" source="#v" offset="0"/>
    <vcount>4</vcount><p>0 1 2 3</p></polylist>
</mesh></geometry></library_geometries>
<library_visual_scenes><visual_scene id="s"><node><translate>0 0 200</translate>
  <instance_geometry url="#quad"><bind_material><technique_common>
    <instance_material symbol="sym" target="#red"/></technique_common></bind_material></instance_geometry>
</node></visual_scene></library_visual_scenes>
<scene><instance_visual_scene url="#s"/></scene></COLLADA>'''


def test_collada_mesh_units_transform_and_material(tmp_path):
    (tmp_path / "quad.dae").write_text(DAE)
    (tmp_path / "r.urdf").write_text('<robot name="r"><link name="l"><visual><geometry>'
        '<mesh filename="quad.dae"/></geometry></visual></link></robot>')
    stage = lightusd.import_urdf(tmp_path / "r.urdf")
    m = mesh(stage)
    points = np.asarray(m["points"])
    # unit meter=0.01 scales the node translate and the vertices; up_axis is
    # reported, not applied (urdf-usd-converter / ROS convention).
    assert points.max(axis=0) == pytest.approx([1, 1, 2])
    assert points.min(axis=0) == pytest.approx([0, 0, 2])
    assert len(m["faceVertexIndices"]) == 6  # quad fan-triangulated
    assert "up_axis Y_UP is not applied" in stage.warnings()
    surfaces = [p for p in stage.prims_of_type("Shader") if p.get("info:id") == "UsdPreviewSurface"]
    assert any(tuple(s.get("inputs:diffuseColor") or ())[:3] == pytest.approx((1, 0, 0)) for s in surfaces)


def test_collada_rejects_bad_indices(tmp_path):
    (tmp_path / "bad.dae").write_text(DAE.replace("<p>0 1 2 3</p>", "<p>0 1 2 9</p>"))
    (tmp_path / "r.urdf").write_text('<robot name="r"><link name="l"><visual><geometry>'
        '<mesh filename="bad.dae"/></geometry></visual></link></robot>')
    with pytest.raises(Exception, match="out of range"):
        lightusd.import_urdf(tmp_path / "r.urdf")


def test_urdf_ghost_links_collapse_like_urdf_usd_converter():
    inertial = b'<inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial>'
    urdf = (b'<robot name="r"><link name="base_footprint"/><link name="base">' + inertial +
            b'</link><link name="tool0"/><link name="tip">' + inertial + b'</link>'
            b'<joint name="fp" type="fixed"><parent link="base_footprint"/><child link="base"/>'
            b'<origin xyz="0 0 .1"/></joint>'
            b'<joint name="flange" type="fixed"><parent link="base"/><child link="tool0"/>'
            b'<origin xyz=".5 0 0" rpy="0 0 1.5707963"/></joint>'
            b'<joint name="j" type="revolute"><parent link="tool0"/><child link="tip"/>'
            b'<origin xyz=".2 0 0"/><axis xyz="0 0 1"/><limit lower="-1" upper="1"/></joint></robot>')
    stage = lightusd.load_bytes(urdf, format='urdf')
    for ghost in ('base_footprint', 'tool0'):
        prim = stage.prim_at(f'/World/Links/{ghost}')
        assert 'PhysicsRigidBodyAPI' not in (prim.metadata('apiSchemas') or ())
    names = {p.name for p in stage.prim_at('/World/Joints').children}
    assert 'flange' not in names
    # The root ghost's joint re-anchors on the world (no body0).
    assert 'physics:body0' not in stage.prim_at('/World/Joints/fp').relationships
    # j re-anchors on `base`: tool0's frame is folded into localPos0/Rot0.
    j = stage.prim_at('/World/Joints/j')
    assert j.relationship('physics:body0').targets == ('/World/Links/base',)
    assert tuple(j['physics:localPos0']) == pytest.approx((.5, .2, 0), abs=1e-6)
    tip = np.asarray(stage.prim_at('/World/Links/tip').world_transform())[3, :3]
    assert tip == pytest.approx([.5, .2, .1], abs=1e-6)
    assert stage.import_report()['counts']['ghost_links'] == 2
    legacy = lightusd._import(data=urdf, format='urdf', profile='legacy')
    assert 'flange' in {p.name for p in legacy.prim_at('/World/Joints').children}


def test_urdf_dummy_inertia_child_merges_into_parent():
    urdf = (b'<robot name="r"><link name="base"><visual><geometry><box size="1 1 1"/></geometry></visual></link>'
            b'<link name="inertia"><inertial><origin xyz=".05 0 0"/><mass value="2"/>'
            b'<inertia ixx="1" iyy="2" izz="3" ixy="0" ixz="0" iyz="0"/></inertial></link>'
            b'<joint name="lump" type="fixed"><parent link="base"/><child link="inertia"/>'
            b'<origin xyz="1 2 3" rpy="0 0 1.5707963267948966"/></joint></robot>')
    stage = lightusd.load_bytes(urdf, format='urdf')
    base = stage.prim_at('/World/Links/base')
    assert base['physics:mass'] == pytest.approx(2)
    assert tuple(base['physics:centerOfMass']) == pytest.approx((1, 2.05, 3), abs=1e-6)
    # Rotating 90 degrees about Z swaps Ixx and Iyy.
    assert list(base['newton:inertia']) == pytest.approx([2, 1, 3, 0, 0, 0], abs=1e-6)
    with pytest.raises(KeyError):
        stage.prim_at('/World/Links/inertia')
    assert stage.import_report()['counts']['merged_dummy_inertia_links'] == 1


def test_typed_mesh_path_keeps_urdf_and_mjcf_geometry():
    # The C/Python import passes mesh arrays as typed buffers (meshRef);
    # every generated visual must still be authored.
    urdf = (b'<robot name="r"><link name="b"><inertial><mass value="1"/>'
            b'<inertia ixx="1" iyy="1" izz="1" ixy="0" ixz="0" iyz="0"/></inertial>'
            b'<visual><geometry><box size="1 2 3"/></geometry></visual>'
            b'<visual><geometry><sphere radius=".5"/></geometry></visual></link></robot>')
    # (Tessellated primitive visuals are the legacy profile's form.)
    legacy = lightusd._import(data=urdf, format='urdf', profile='legacy')
    meshes = [p for p in legacy if p.type_name == 'Mesh']
    assert len(meshes) == 2 and all(len(m['points']) > 8 for m in meshes)
    mjcf = (b'<mujoco><worldbody><body name="b"><freejoint/>'
            b'<geom type="hfield" hfield="h"/></body></worldbody>'
            b'<asset><hfield name="h" nrow="3" ncol="3" size="1 1 .1 .1" elevation="0 1 0 1 0 1 0 1 0"/></asset></mujoco>')
    hfield = [p for p in lightusd.load_bytes(mjcf, format='mjcf') if p.type_name == 'Mesh']
    assert hfield and len(hfield[0]['points']) >= 9


def test_parallel_mesh_preload_matches_serial_import(tmp_path):
    import json
    import struct
    tri = struct.pack("<12fH", 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0)
    for i in range(4):
        (tmp_path / f"s{i}.stl").write_bytes(b"\0" * 80 + struct.pack("<I", 1) + tri)
    (tmp_path / "red.mtl").write_text("newmtl red\nKd 1 0 0\n")
    obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
    (tmp_path / "colored.obj").write_text("mtllib red.mtl\nusemtl red\n" + obj)
    (tmp_path / "missing.obj").write_text("mtllib nowhere.mtl\n" + obj)
    (tmp_path / "unused.obj").write_text(obj)
    assets = "".join(f'<mesh name="s{i}" file="s{i}.stl"/>' for i in range(4))
    assets += '<mesh file="colored.obj"/><mesh file="missing.obj"/><mesh file="unused.obj"/>'
    geoms = "".join(f'<geom type="mesh" mesh="{n}"/>' for n in ("s3", "colored", "s1", "missing", "s0", "s2"))
    (tmp_path / "m.xml").write_text(f'<mujoco><asset>{assets}</asset><worldbody><body>'
                                    f'<freejoint/>{geoms}</body></worldbody></mujoco>')

    def run(threads, **kw):
        try:
            stage = lightusd.import_mjcf(tmp_path / "m.xml", threads=threads, **kw)
        except Exception as e:  # noqa: BLE001 - compare failures too
            return type(e).__name__, str(e)
        report = stage.import_report()
        report.pop("elapsed_seconds", None)
        return stage.export_usda(), stage.warnings(), json.dumps(report, sort_keys=True)

    serial = run(1)
    assert "nowhere.mtl" in serial[1]
    assert run(0) == serial and run(4) == serial
    for budget in (1 << 16, 1 << 18):
        assert run(0, max_memory=budget) == run(1, max_memory=budget)
    with pytest.raises(ValueError):
        lightusd.import_mjcf(tmp_path / "m.xml", threads=-1)


def test_urdf_mesh_preload_matches_serial_and_reads_stl_once(tmp_path):
    import json
    import struct
    tri = struct.pack("<12fH", 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0)
    for i in range(3):
        (tmp_path / f"s{i}.stl").write_bytes(b"\0" * 80 + struct.pack("<I", 1) + tri)
    (tmp_path / "red.mtl").write_text("newmtl red\nKd 1 0 0\n")
    obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
    (tmp_path / "a.obj").write_text("mtllib red.mtl\nusemtl red\n" + obj)
    (tmp_path / "b.obj").write_text("mtllib nowhere.mtl\n" + obj)
    (tmp_path / "q.dae").write_text(DAE)
    files = ["s0.stl", "a.obj", "s1.stl", "b.obj", "q.dae", "s2.stl", "a.obj"]
    links = "".join(f'<link name="l{i}"><visual><geometry><mesh filename="{f}"/></geometry></visual>'
                    f'<collision><geometry><mesh filename="{f}"/></geometry></collision></link>'
                    for i, f in enumerate(files))
    joints = "".join(f'<joint name="j{i}" type="fixed"><parent link="l{i - 1}"/><child link="l{i}"/></joint>'
                     for i in range(1, len(files)))
    (tmp_path / "r.urdf").write_text(f'<robot name="r">{links}{joints}</robot>')

    def run(threads, **kw):
        try:
            stage = lightusd.import_urdf(tmp_path / "r.urdf", threads=threads, **kw)
        except Exception as e:  # noqa: BLE001 - compare failures too
            return type(e).__name__, str(e)
        report = stage.import_report()
        report.pop("elapsed_seconds", None)
        return stage.export_usda(), stage.warnings(), json.dumps(report, sort_keys=True)

    serial = run(1)
    assert run(0) == serial
    for budget in (1 << 15, 1 << 17):
        assert run(0, max_memory=budget) == run(1, max_memory=budget)
    deps = [d for d in json.loads(serial[2])["dependencies"] if d["identifier"].endswith(".stl")]
    assert len(deps) == 3  # each STL is read once, though visual and collision use it
