# SPDX-License-Identifier: Apache-2.0
"""Newton-profile details checked against Newton's USD importer contract."""
import math

import pytest

import lightusd


def _mjcf(body, extra=''):
    return f'<mujoco>{extra}<worldbody>{body}</worldbody></mujoco>'.encode()


def test_urdf_planar_free_axes_are_unlimited_dofs():
    urdf = b'''<robot name="r"><link name="base"/><link name="s">
      <inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link>
      <joint name="p" type="planar"><parent link="base"/><child link="s"/><axis xyz="0 0 1"/></joint></robot>'''
    plane = lightusd.load_bytes(urdf, format='urdf').prim_at('/World/Joints/p')
    apis = set(plane.metadata('apiSchemas'))
    for dof in ('transY', 'transZ', 'rotX'):
        assert f'PhysicsLimitAPI:{dof}' in apis
        assert plane[f'limit:{dof}:physics:low'] == -math.inf
        assert plane[f'limit:{dof}:physics:high'] == math.inf


def test_urdf_effort_becomes_joint_force_range_and_no_zero_gap():
    urdf = b'''<robot name="r"><link name="a"><collision><geometry><box size="1 1 1"/></geometry></collision></link>
      <link name="b"><inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial></link>
      <joint name="j" type="revolute"><parent link="a"/><child link="b"/><axis xyz="0 0 1"/>
      <limit lower="-1" upper="1" effort="12" velocity="2"/></joint></robot>'''
    stage = lightusd.load_bytes(urdf, format='urdf')
    j = stage.prim_at('/World/Joints/j')
    assert j['mjc:actuatorfrclimited'] == 'true'
    assert (j['mjc:actuatorfrcrange:min'], j['mjc:actuatorfrcrange:max']) == (-12, 12)
    collider = next(p for p in stage if p.type_name == 'Cube')
    assert 'newton:contactGap' not in collider.attributes
    assert 'newton:contactMargin' not in collider.attributes


def test_scene_flags_gravity_and_filterparent():
    xml = _mjcf('<body name="a"><joint name="h" axis="0 0 1"/><geom size=".1"/></body>',
                '<option><flag gravity="disable" filterparent="disable" spring="disable"/></option>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    scene = stage.prim_at('/World/PhysicsScene')
    assert scene['newton:gravityEnabled'] is False
    assert scene['mjc:flag:spring'] is False
    assert stage.prim_at('/World/Joints/h')['physics:collisionEnabled'] is True


def test_ref_is_not_mirrored_into_joint_state():
    xml = _mjcf('<body name="a"><joint name="h" axis="0 0 1" ref="30"/><geom size=".1"/></body>')
    j = lightusd.load_bytes(xml, format='mjcf').prim_at('/World/Joints/h')
    assert 'state:angular:physics:position' not in j.attributes
    assert j['mjc:ref'] == pytest.approx(30)


def test_weld_relpose_anchors_body2_frame():
    xml = _mjcf('<body name="a" pos="0 0 1"><freejoint/><geom size=".1"/></body>'
                '<body name="b" pos="3 0 1"><freejoint/><geom size=".1"/></body>',
                ).replace(b'</mujoco>', b'<equality><weld body1="a" body2="b" anchor=".1 .2 .3" '
                          b'relpose="1 0 0 1 0 0 0"/></equality></mujoco>')
    weld = next(p for p in lightusd.load_bytes(xml, format='mjcf')
                if p.type_name == 'PhysicsFixedJoint')
    assert tuple(weld['physics:localPos1']) == pytest.approx((.1, .2, .3))
    assert tuple(weld['physics:localPos0']) == pytest.approx((1.1, .2, .3))


def test_stack_with_ball_keeps_chain_in_newton_profile():
    xml = _mjcf('<body name="a"><joint name="j0" type="ball"/><joint name="j1" type="slide"/>'
                '<geom size=".1"/></body>')
    joints = [p for p in lightusd.load_bytes(xml, format='mjcf') if p.path.startswith('/World/Joints/')]
    bodies = {p.relationship('physics:body1').targets for p in joints}
    assert len(joints) == 2 and len(bodies) == 2  # one intermediate link
    stacked = lightusd._import(data=xml, format='mjcf', profile='openusd')
    joints = [p for p in stacked if p.path.startswith('/World/Joints/')]
    assert len({p.relationship('physics:body1').targets for p in joints}) == 1


def test_unresolved_actuator_target_is_skipped():
    xml = _mjcf('<body name="a"><joint name="h"/><geom size=".1"/></body>',
                ).replace(b'</mujoco>', b'<tendon><spatial name="t"><site site="nope"/><site site="x"/>'
                          b'</spatial></tendon><actuator><motor name="m" tendon="t"/></actuator></mujoco>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    assert not [p for p in stage if p.type_name == 'MjcActuator']
    assert 'Skipping actuator `m`' in stage.warnings()


def test_exclude_with_world_filters_world_colliders():
    xml = _mjcf('<geom name="floor" type="plane" size="1 1 .1"/>'
                '<body name="a" pos="0 0 1"><freejoint/><geom size=".1"/></body>'
                ).replace(b'</mujoco>', b'<contact><exclude body1="world" body2="a"/></contact></mujoco>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    a = stage.prim_at('/World/Links/a')
    targets = a.relationship('physics:filteredPairs').targets
    assert targets and all(stage.prim_at(t).type_name == 'Plane' for t in targets)


def test_nested_layout_follows_the_kinematic_tree():
    import numpy as np
    xml = _mjcf('<body name="a" pos="0 0 1" euler="0 0 30"><freejoint/><geom size=".1"/>'
                '<body name="b" pos=".5 0 0" euler="10 0 0"><joint name="j1" axis="0 1 0"/><geom name="c" size=".05"/>'
                '<site name="s" pos="0 0 .1"/>'
                '<body name="c" pos="0 .5 0"><joint name="j2"/><geom size=".05"/></body></body></body>')
    xml = xml.replace(b'</mujoco>', b'<tendon><spatial name="t"><site site="s"/><site site="s2"/></spatial></tendon></mujoco>')
    xml = xml.replace(b'<site name="s" pos="0 0 .1"/>', b'<site name="s" pos="0 0 .1"/><site name="s2" pos=".1 0 0"/>')
    flat = lightusd.load_bytes(xml, format='mjcf')
    nested = lightusd._import(data=xml, format='mjcf', layout='nested')
    for name, path in (('a', '/World/Links/a'), ('b', '/World/Links/a/b'), ('c', '/World/Links/a/b/c')):
        assert np.asarray(nested.prim_at(path).world_transform()) == pytest.approx(
            np.asarray(flat.prim_at(f'/World/Links/{name}').world_transform()), abs=1e-9)
    # The geom named like a child link does not collide with it.
    assert nested.prim_at('/World/Links/a/b').child('c').type_name == 'Xform'
    assert nested.prim_at('/World/Joints/j2').relationship('physics:body1').targets == ('/World/Links/a/b/c',)
    sites = nested.prim_at('/World/Tendons/t').relationship('mjc:path').targets
    assert sites[0] == '/World/Links/a/b/s'
    with pytest.raises(ValueError):
        lightusd._import(data=xml, format='mjcf', layout='tree')


def test_nested_layout_urdf_with_ghost_frame():
    urdf = b'''<robot name="r"><link name="base"><inertial><mass value="1"/><inertia ixx="1" iyy="1" izz="1" ixy="0" ixz="0" iyz="0"/></inertial></link>
      <link name="tool0"/><link name="tip"><inertial><mass value="1"/><inertia ixx="1" iyy="1" izz="1" ixy="0" ixz="0" iyz="0"/></inertial></link>
      <joint name="f" type="fixed"><parent link="base"/><child link="tool0"/><origin xyz="1 0 0"/></joint>
      <joint name="j" type="revolute"><parent link="tool0"/><child link="tip"/><origin xyz="0 1 0"/><axis xyz="0 0 1"/><limit lower="-1" upper="1"/></joint></robot>'''
    stage = lightusd._import(data=urdf, format='urdf', layout='nested')
    joint = stage.prim_at('/World/Joints/j')
    tip = stage.prim_at(joint.relationship('physics:body1').targets[0])
    assert tuple(round(v, 6) for v in tip.world_transform()[3][:3]) == (1, 1, 0)


_INERTIAL = '<inertial><mass value="1"/><inertia ixx=".1" iyy=".1" izz=".1" ixy="0" ixz="0" iyz="0"/></inertial>'


def test_urdf_transmission_is_joint_metadata_not_an_actuator():
    urdf = f'''<robot name="r"><link name="a"/><link name="b">{_INERTIAL}</link>
      <joint name="j" type="revolute"><parent link="a"/><child link="b"/><axis xyz="0 0 1"/>
      <limit lower="-1" upper="1"/></joint>
      <transmission name="t"><type>transmission_interface/SimpleTransmission</type>
      <joint name="j"><hardwareInterface>hardware_interface/EffortJointInterface</hardwareInterface></joint>
      <actuator name="m"><mechanicalReduction>50</mechanicalReduction></actuator></transmission></robot>'''.encode()
    stage = lightusd.load_bytes(urdf, format='urdf')
    assert not [p for p in stage if p.type_name == 'NewtonActuator']
    j = stage.prim_at('/World/Joints/j')
    assert j['urdf:transmission:name'] == 't'
    assert j['urdf:transmission:type'] == 'transmission_interface/SimpleTransmission'
    assert j['urdf:transmission:actuator'] == 'm'
    assert j['urdf:transmission:hardwareInterface'] == 'hardware_interface/EffortJointInterface'
    assert j['urdf:transmission:mechanicalReduction'] == 50
    legacy = lightusd._import(data=urdf, format='urdf', profile='legacy')
    assert [p for p in legacy if p.type_name == 'NewtonActuator']


def _write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


_TRI_OBJ = 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n'


def _package_urdf(mesh):
    return f'''<robot name="r"><link name="a"><visual><geometry><mesh filename="{mesh}"/></geometry></visual>
      {_INERTIAL}</link></robot>'''


def test_urdf_package_found_by_directory_name(tmp_path):
    _write(tmp_path / 'my_pkg' / 'meshes' / 'm.obj', _TRI_OBJ)
    urdf = _write(tmp_path / 'my_pkg' / 'urdf' / 'r.urdf', _package_urdf('package://my_pkg/meshes/m.obj'))
    stage = lightusd.import_urdf(urdf)
    assert [p for p in stage if p.type_name == 'Mesh']
    codes = [d['code'] for d in stage.import_report()['diagnostics']]
    assert 'urdf.package_discovered' in codes


def test_urdf_package_found_by_relative_path(tmp_path):
    # The checkout directory is not named like the package.
    _write(tmp_path / 'checkout' / 'meshes' / 'm.obj', _TRI_OBJ)
    urdf = _write(tmp_path / 'checkout' / 'urdf' / 'r.urdf', _package_urdf('package://other/meshes/m.obj'))
    assert [p for p in lightusd.import_urdf(urdf) if p.type_name == 'Mesh']
    # An explicit mapping wins; `..` paths are never searched.
    _write(tmp_path / 'outside.obj', _TRI_OBJ)
    escape = _write(tmp_path / 'checkout' / 'urdf' / 'e.urdf', _package_urdf('package://other/../outside.obj'))
    with pytest.raises(lightusd.UsdError):
        lightusd.import_urdf(escape)


def test_self_collision_is_only_authored_when_the_source_sets_it():
    xml = _mjcf('<body name="a"><freejoint/><geom size=".1"/><body name="b"><joint axis="0 0 1"/>'
                '<geom size=".1"/></body></body>')
    roots = lambda s: [p for p in s if 'PhysicsArticulationRootAPI' in (p.metadata('apiSchemas') or ())]
    newton = roots(lightusd.load_bytes(xml, format='mjcf'))
    assert newton and all('newton:selfCollisionEnabled' not in p.attributes for p in newton)
    legacy = roots(lightusd._import(data=xml, format='mjcf', profile='legacy'))
    assert legacy and all(p['newton:selfCollisionEnabled'] is True for p in legacy)


def test_option_iterations_is_newton_solver_iterations():
    xml = _mjcf('<body name="a"><freejoint/><geom size=".1"/></body>', '<option iterations="17"/>')
    assert lightusd.load_bytes(xml, format='mjcf').prim_at('/World/PhysicsScene')[
        'newton:maxSolverIterations'] == 17


def _mesh_colliders(stage):
    return {p.name: p for p in stage
            if 'PhysicsMeshCollisionAPI' in (p.metadata('apiSchemas') or ())}


def test_mesh_inertia_modes_and_hull_limit(tmp_path):
    tet = 'v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n'
    _write(tmp_path / 'tet.obj', tet)
    assets = ''.join(f'<mesh name="{m}" file="tet.obj" inertia="{m}"/>'
                     for m in ('shell', 'exact', 'convex', 'legacy'))
    assets += '<mesh name="capped" file="tet.obj" maxhullvert="8"/>'
    geoms = ''.join(f'<geom name="{m}" type="mesh" mesh="{m}"/>'
                    for m in ('shell', 'exact', 'convex', 'legacy', 'capped'))
    xml = _mjcf(f'<body name="a"><freejoint/>{geoms}<geom name="ball" size=".1" shellinertia="true"/></body>',
                f'<asset>{assets}</asset>')
    path = tmp_path / 'm.xml'
    path.write_bytes(xml)
    stage = lightusd.import_mjcf(path)
    colliders = _mesh_colliders(stage)
    by_geom = {name: next(p for n, p in colliders.items() if n.startswith(name)) for name in
               ('shell', 'exact', 'convex', 'legacy', 'capped')}
    assert by_geom['shell']['newton:massModel'] == 'shell'
    assert 'mjc:inertia' not in by_geom['shell'].attributes
    assert by_geom['exact']['mjc:inertia'] == 'exact'
    assert by_geom['legacy']['mjc:inertia'] == 'legacy'
    assert 'mjc:inertia' not in by_geom['convex'].attributes
    assert by_geom['capped']['newton:maxHullVertices'] == 8
    assert all('newton:maxHullVertices' not in by_geom[n].attributes
               for n in ('shell', 'exact', 'convex', 'legacy'))
    ball = next(p for p in stage if p.type_name == 'Sphere' and
                'PhysicsCollisionAPI' in (p.metadata('apiSchemas') or ()))
    assert ball['newton:massModel'] == 'shell'
    legacy = _mesh_colliders(lightusd.import_mjcf(path, profile='legacy'))
    assert all(p['newton:maxHullVertices'] in (8, 64) for p in legacy.values())


def test_physics_material_contact_stiffness_from_solref():
    xml = _mjcf('<body name="a"><freejoint/><geom name="g1" size=".1"/>'
                '<geom name="g2" size=".1" pos=".3 0 0" solref=".01 .5"/>'
                '<geom name="g3" size=".1" pos=".6 0 0" solref="-1000 -20"/></body>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    materials = [p for p in stage if 'PhysicsMaterialAPI' in (p.metadata('apiSchemas') or ())]
    gains = sorted((m['newton:contactStiffness'], m['newton:contactDamping']) for m in materials)
    assert gains == [pytest.approx((1000, 20)), pytest.approx((2500, 100)),
                     pytest.approx((40000, 200))]


def _apis(prim):
    return set(prim.metadata('apiSchemas') or ())


def test_visible_collider_is_one_prim():
    xml = _mjcf('<body name="a"><freejoint/><geom name="g" size=".1" rgba="1 0 0 1" material="m"/>'
                '<geom name="hidden" size=".1" group="3"/></body>',
                '<asset><material name="m" rgba="0 1 0 1"/></asset>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    g = stage.prim_at('/World/Links/a/g')
    assert 'PhysicsCollisionAPI' in _apis(g)
    assert 'purpose' not in g.attributes
    assert g.relationship('material:binding').targets == ('/World/Materials/m',)
    assert g.relationship('material:binding:physics').targets
    assert tuple(g['primvars:displayColor'][0]) == (1, 0, 0)
    assert not stage.get_prim_at('/World/Links/a/g1')
    assert stage.prim_at('/World/Links/a/hidden')['purpose'] == 'guide'


def test_jointless_urdf_has_no_articulation_root():
    urdf = f'<robot name="r"><link name="a">{_INERTIAL}</link></robot>'.encode()
    stage = lightusd.load_bytes(urdf, format='urdf')
    assert not [p for p in stage if 'PhysicsArticulationRootAPI' in _apis(p)]


def test_deprecated_mjc_aliases_are_not_authored():
    xml = _mjcf('<body name="a"><freejoint/><geom size=".1" margin=".01" gap=".002"/>'
                '<body name="b"><joint name="j1" axis="0 0 1"/><geom size=".1"/></body>'
                '<body name="c"><joint name="j2" axis="0 0 1"/><geom size=".1"/></body></body>',
                '<option timestep="0.002" iterations="20"><flag gravity="disable"/></option>')
    xml = xml.replace(b'</mujoco>', b'<equality><joint joint1="j2" joint2="j1"/></equality></mujoco>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    scene = stage.prim_at('/World/PhysicsScene')
    for name in ('mjc:option:timestep', 'mjc:option:iterations', 'mjc:flag:gravity', 'mjc:timestep'):
        assert name not in scene.attributes
    assert scene['newton:timeStepsPerSecond'] == 500
    collider = next(p for p in stage if 'PhysicsCollisionAPI' in _apis(p))
    assert 'mjc:margin' not in collider.attributes and 'mjc:gap' not in collider.attributes
    assert collider['newton:contactMargin'] == pytest.approx(.01)
    j2 = stage.prim_at('/World/Joints/j2')
    assert j2.relationship('newton:mimicJoint').targets == ('/World/Joints/j1',)
    assert 'mjc:target' not in j2.relationships
    # A timestep newton:timeStepsPerSecond cannot hold keeps the MJC value.
    odd = lightusd.load_bytes(_mjcf('<body name="a"><freejoint/><geom size=".1"/></body>',
                                    '<option timestep="0.0015"/>'), format='mjcf')
    assert odd.prim_at('/World/PhysicsScene')['mjc:option:timestep'] == pytest.approx(0.0015)


def test_mjcf_material_color_is_linear_and_shininess_is_roughness():
    xml = _mjcf('<body name="a"><geom size=".1" material="m"/></body>',
                '<asset><material name="m" rgba=".5 .5 .5 1" shininess=".8"/></asset>')
    surface = lightusd.load_bytes(xml, format='mjcf').prim_at('/World/Materials/m/PreviewSurface')
    assert surface['inputs:diffuseColor'][0] == pytest.approx(((.5 + .055) / 1.055) ** 2.4, abs=1e-6)
    assert surface['inputs:roughness'] == pytest.approx(.2)


def test_sanitized_names_keep_display_names():
    xml = _mjcf('<body name="arm-1"><joint name="elbow.j" axis="0 0 1"/><geom name="g-1" size=".1"/></body>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    assert stage.prim_at('/World/Links/arm_1').metadata('displayName') == 'arm-1'
    assert stage.prim_at('/World/Joints/elbow_j').metadata('displayName') == 'elbow.j'
    assert stage.prim_at('/World/Links/arm_1/g_1').metadata('displayName') == 'g-1'


def test_frame_scale_and_actuator_group_disable():
    xml = _mjcf('<frame scale="2 2 2"><body name="a"><freejoint/><geom size=".1"/></body></frame>',
                '<option actuatorgroupdisable="1 3"/>')
    stage = lightusd.load_bytes(xml, format='mjcf')
    codes = [d['code'] for d in stage.import_report()['diagnostics']]
    assert 'mjcf.frame_scale_unsupported' in codes
    assert list(stage.prim_at('/World/PhysicsScene')['mjc:option:actuatorgroupdisable']) == [1, 3]


def test_urdf_unknown_elements_and_capsule():
    urdf = f'''<robot name="r" xmlns:xacro="http://www.ros.org/wiki/xacro">
      <link name="a" custom="1">{_INERTIAL}<visual><geometry><capsule radius=".1" length=".4"/></geometry></visual>
      <collision><geometry><box size="1 1 1"/></geometry></collision><sensor_info rate="30">imu</sensor_info></link>
      <link name="b">{_INERTIAL}<visual><geometry><box size="1 2 3"/></geometry></visual></link>
      <joint name="j" type="revolute" vendor="x"><parent link="a"/><child link="b"/><axis xyz="0 0 1"/>
      <limit lower="-1" upper="1"/></joint>
      <gazebo reference="a"><material>Gazebo/Red</material></gazebo></robot>'''.encode()
    stage = lightusd.load_bytes(urdf, format='urdf')
    gazebo = stage.prim_at('/World/custom/gazebo')
    assert gazebo['urdf:reference'] == 'a'
    assert stage.prim_at('/World/custom/gazebo/material')['urdf:text'] == 'Gazebo/Red'
    a = stage.prim_at('/World/Links/a')
    assert a['urdf:custom'] == '1'
    sensor = stage.prim_at('/World/Links/a/sensor_info')
    assert (sensor['urdf:rate'], sensor['urdf:text']) == ('30', 'imu')
    assert stage.prim_at('/World/Joints/j')['urdf:vendor'] == 'x'
    capsule = next(p for p in stage if p.type_name == 'Capsule')
    assert (capsule['radius'], capsule['height']) == (pytest.approx(.1), pytest.approx(.4))
    assert [p for p in stage if p.type_name == 'Cube' and 'PhysicsCollisionAPI' not in _apis(p)]
