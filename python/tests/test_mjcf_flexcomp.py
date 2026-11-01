# SPDX-License-Identifier: Apache-2.0
"""MJCF flexcomp soft bodies as UsdPhysics deformables (newton profile)."""
import json
import numpy as np
import pytest

import lightusd

FLEX = '''<mujoco><worldbody>
  <body name="holder" pos="0 0 1">
    <flexcomp name="%s" type="grid" dim="%d" count="%s" spacing=".1 .1 .1" mass="2" radius=".01"
              pos=".1 0 0" euler="0 0 90">
      <elasticity young="1e4" poisson=".3" thickness=".002" elastic2d="both"/>
      <edge stiffness="50" damping=".5"/>
      %s
    </flexcomp>
  </body></worldbody></mujoco>'''


def _stage(name, dim, count, extra='', profile='newton'):
    return lightusd._import(data=(FLEX % (name, dim, count, extra)).encode(), format='mjcf',
                            profile=profile)


def _points(prim):
    return np.asarray(prim['points'], dtype=float).reshape(-1, 3)


def _material(stage, prim):
    return stage.prim_at(prim.relationship('material:binding:physics').targets[0])


def test_cloth_grid_matches_mujoco_layout_and_pins():
    stage = _stage('cloth', 2, '3 4 1', '<pin id="0 11"/>')
    cloth = stage.prim_at('/World/Deformables/cloth')
    assert cloth.type_name == 'Mesh'
    assert 'PhysicsSurfaceDeformableSimAPI' in cloth.metadata('apiSchemas')
    assert len(cloth['faceVertexCounts']) == 2 * 2 * 3
    pts = _points(cloth)
    # Grid point (ix, iy) = spacing * (i - (count - 1) / 2), rotated 90 deg
    # about Z and placed at the flexcomp pose inside the body.
    assert pts[0] == pytest.approx([.1 + .15, -.1, 1])
    assert list(cloth['physics:masses']) == pytest.approx([2 / 12] * 12)
    mat = _material(stage, cloth)
    assert mat['physics:youngsModulus'] == pytest.approx(1e4)
    assert mat['physics:surfaceStretchStiffness'] == pytest.approx(1e4 * .002 / (1 - .09))
    pins = stage.prim_at('/World/Deformables/cloth_pins')
    assert pins.type_name == 'PhysicsAttachment'
    assert list(pins['physics:indices0']) == [0, 11]
    assert pins.relationship('physics:src1').targets == ('/World/Links/holder',)
    # Pin coordinates are in the body frame.
    assert tuple(pins['physics:coords1'][0]) == pytest.approx((.25, -.1, 0))


def test_cable_and_volume():
    stage = _stage('rope', 1, '5 1 1')
    rope = stage.prim_at('/World/Deformables/rope')
    assert rope.type_name == 'BasisCurves' and list(rope['curveVertexCounts']) == [5]
    mat = _material(stage, rope)
    assert mat['physics:curvesStretchStiffness'] == pytest.approx(50 * .1)
    assert mat['newton:curvesStretchDamping'] == pytest.approx(.5 * .1)
    assert list(rope['physics:thicknesses']) == pytest.approx([.02])

    stage = _stage('jelly', 3, '3 3 3')
    jelly = stage.prim_at('/World/Deformables/jelly')
    tets = np.asarray(jelly['tetVertexIndices']).reshape(-1, 4)
    assert len(tets) == 8 * 6
    pts = _points(jelly)
    vol = [np.linalg.det(np.stack([pts[t[1]] - pts[t[0]], pts[t[2]] - pts[t[0]],
                                   pts[t[3]] - pts[t[0]]])) for t in tets]
    assert min(vol) > 0  # Newton keeps only right-handed tets
    assert len(jelly['surfaceFaceVertexIndices']) == 6 * 2 * 2 * 2


def test_flexcomp_outside_newton_profile_is_unsupported():
    stage = _stage('cloth', 2, '3 3 1', profile='openusd')
    with pytest.raises(KeyError):
        stage.prim_at('/World/Deformables')
    assert 'Unsupported MJCF element: flexcomp' in stage.warnings()


@pytest.mark.parametrize('dof', ['radial', 'trilinear', 'quadratic', '2d'])
def test_valid_reduced_dof_reports_approximation(dof):
    xml = (FLEX % ('flex', 3, '3 3 3', '')).replace('type="grid"', f'type="grid" dof="{dof}"')
    stage = lightusd.load_bytes(xml.encode(), format='mjcf')
    assert stage.prim_at('/World/Deformables/flex').type_name == 'TetMesh'
    assert f'dof=`{dof}` is approximated' in stage.warnings()
    with pytest.raises(lightusd.UsdError, match='approximated'):
        lightusd.load_bytes(xml.encode(), format='mjcf', unsupported_policy='error')
    preserved = lightusd.load_bytes(xml.encode(), format='mjcf', unsupported_policy='preserve')
    records = json.loads(preserved.custom_layer_data['lightusdImportUnsupported'])
    assert any(record['feature'] == 'mjcf.flexcomp' and f'dof="{dof}"' in record['data']
               and not record['executable'] for record in records)


@pytest.mark.parametrize('dof', ['trilnear', '3d', 'bogus'])
@pytest.mark.parametrize('policy', ['warn', 'error', 'preserve'])
def test_invalid_flex_dof_is_rejected(dof, policy):
    xml = (FLEX % ('flex', 3, '3 3 3', '')).replace('type="grid"', f'type="grid" dof="{dof}"')
    with pytest.raises(lightusd.UsdError, match='invalid dof'):
        lightusd.load_bytes(xml.encode(), format='mjcf', unsupported_policy=policy)


@pytest.mark.parametrize('attribute,value', [
    ('dim', '2.5'), ('dim', '2147483648'),
    ('count', '3.5 3 1'), ('count', '3 3'),
    ('count', '3 3 1 1'), ('count', '2147483648 3 1'),
])
def test_invalid_flex_grid_integers_are_rejected(attribute, value):
    xml = (FLEX % ('flex', 2, '3 3 1', '')).replace(
        f'{attribute}="' + ('2' if attribute == 'dim' else '3 3 1') + '"',
        f'{attribute}="{value}"')
    with pytest.raises(lightusd.UsdError, match=attribute):
        lightusd.load_bytes(xml.encode(), format='mjcf')


@pytest.mark.parametrize('shape', ['circle', 'square', 'disc'])
def test_shape_override_does_not_hide_invalid_dimension(shape):
    xml = (FLEX % ('flex', 4, '3 3 1', '')).replace('type="grid"', f'type="{shape}"')
    with pytest.raises(lightusd.UsdError, match='dim'):
        lightusd.load_bytes(xml.encode(), format='mjcf')


@pytest.mark.parametrize('position', ['1e39 0 0', '1e308 1e308 1e308'])
def test_flex_points_outside_float_range_are_rejected(position):
    xml = (FLEX % ('flex', 2, '3 3 1', '')).replace('pos=".1 0 0"', f'pos="{position}"')
    with pytest.raises(lightusd.UsdError, match='float range'):
        lightusd.load_bytes(xml.encode(), format='mjcf')


def test_flexcomp_matches_mujoco_vertices():
    mujoco = pytest.importorskip('mujoco')
    # MuJoCo accepts edge stiffness only for 1D flexes.
    xml = (FLEX % ('cloth', 2, '4 3 1', '<pin id="2"/>')).replace(
        '<edge stiffness="50" damping=".5"/>', '')
    model = mujoco.MjModel.from_xml_string(xml)
    data = mujoco.MjData(model)
    mujoco.mj_kinematics(model, data)
    mujoco.mj_flex(model, data)
    ours = _points(lightusd.load_bytes(xml.encode(), format='mjcf').prim_at('/World/Deformables/cloth'))
    np.testing.assert_allclose(ours, data.flexvert_xpos, atol=1e-6)


CABLE = '''<mujoco><worldbody><body name="hook" pos="0 0 1">
  <composite type="cable" prefix="c" curve="s cos(s) 0" count="9 1 1" size="1 .2 1" offset=".1 0 0">
    <joint kind="main" damping=".02" stiffness=".1"/>
    <geom type="capsule" size=".01" density="500"/>
  </composite></body></worldbody></mujoco>'''


def test_cable_composite_expands_like_mujoco():
    stage = lightusd.load_bytes(CABLE.encode(), format='mjcf')
    names = ['cB_first'] + [f'cB_{i}' for i in range(1, 7)] + ['cB_last']
    joints = [p for p in stage if p.type_name == 'PhysicsSphericalJoint']
    assert len(joints) == 8  # initial="ball": the first segment hangs on a ball too
    mujoco = pytest.importorskip('mujoco')
    model = mujoco.MjModel.from_xml_string(CABLE)
    data = mujoco.MjData(model)
    mujoco.mj_kinematics(model, data)
    for name in names:
        prim = stage.prim_at(f'/World/Links/{name}')
        body = model.body(name)
        m = np.asarray(prim.world_transform())
        np.testing.assert_allclose(m[3, :3], data.xpos[body.id], atol=1e-6)
        # USD row-vector rotation is MuJoCo's xmat transposed.
        np.testing.assert_allclose(m[:3, :3], data.xmat[body.id].reshape(3, 3).T, atol=1e-6)
        assert prim['physics:mass'] == pytest.approx(model.body_mass[body.id], rel=1e-5)
    assert sum(1 for p in stage if p.path.startswith('/World/Links/cB_') and 'Site' not in p.type_name
               and p.name.startswith('cS_')) == 2


CUBE_OBJ = '\n'.join(['v -.1 -.1 -.1', 'v .1 -.1 -.1', 'v .1 .1 -.1', 'v -.1 .1 -.1',
                      'v -.1 -.1 .1', 'v .1 -.1 .1', 'v .1 .1 .1', 'v -.1 .1 .1',
                      'f 1 3 2', 'f 1 4 3', 'f 5 6 7', 'f 5 7 8', 'f 1 2 6', 'f 1 6 5',
                      'f 2 3 7', 'f 2 7 6', 'f 3 4 8', 'f 3 8 7', 'f 4 1 5', 'f 4 5 8']) + '\n'
TET_GMSH = '''$MeshFormat
4.1 0 8
$EndMeshFormat
$Nodes
1 4 1 4
3 1 0 4
1
2
3
4
0 0 0
1 0 0
0 1 0
0 0 1
$EndNodes
$Elements
1 1 1 1
3 1 4 1
1 1 2 3 4
$EndElements
'''


@pytest.mark.parametrize('flexcomp,kind,count', [
    ('type="box" dim="2" count="4 3 5"', 'Mesh', 52),
    ('type="ellipsoid" dim="3" count="5 6 5" euler="10 20 30"', 'TetMesh', 115),
    ('type="mesh" dim="3" file="cube.obj" origin="0 0 0"', 'TetMesh', 9),
    ('type="gmsh" file="tet.msh"', 'TetMesh', 4),
])
def test_box_mesh_and_gmsh_flexcomps(tmp_path, flexcomp, kind, count):
    (tmp_path / 'cube.obj').write_text(CUBE_OBJ)
    (tmp_path / 'tet.msh').write_text(TET_GMSH)
    xml = (f'<mujoco><worldbody><body name="b" pos="0 0 1"><flexcomp name="f" {flexcomp} '
           'spacing=".1 .1 .1" radius=".001"/></body></worldbody></mujoco>')
    path = tmp_path / 'scene.xml'
    path.write_text(xml)
    prim = lightusd.import_mjcf(path).prim_at('/World/Deformables/f')
    assert prim.type_name == kind
    pts = _points(prim)
    assert len(pts) == count
    mujoco = pytest.importorskip('mujoco')
    model = mujoco.MjModel.from_xml_path(str(path))
    data = mujoco.MjData(model)
    mujoco.mj_kinematics(model, data)
    mujoco.mj_flex(model, data)
    np.testing.assert_allclose(pts, data.flexvert_xpos, atol=1e-6)


def test_cable_plugin_bending_becomes_joint_stiffness():
    xml = CABLE.replace('<joint kind="main" damping=".02" stiffness=".1"/>',
                        '<plugin plugin="mujoco.elasticity.cable"><config key="bend" value="1e6"/>'
                        '<config key="twist" value="1e6"/></plugin><joint kind="main" damping=".02"/>')
    stage = lightusd.load_bytes(xml.encode(), format='mjcf')
    joints = [p for p in stage if p.type_name == 'PhysicsSphericalJoint']
    stiff = sorted(p['mjc:stiffness'] for p in joints if 'mjc:stiffness' in p.attributes)
    assert len(stiff) == 7  # every joint except the first segment's
    names = ['cB_first'] + [f'cB_{i}' for i in range(1, 7)] + ['cB_last']
    pos = [np.asarray(stage.prim_at(f'/World/Links/{n}').world_transform())[3, :3] for n in names]
    seg = [np.linalg.norm(b - a) for a, b in zip(pos, pos[1:])]
    expected = sorted(1e6 * np.pi * .01 ** 4 / 4 / s for s in seg)
    assert stiff == pytest.approx(expected, rel=1e-5)
    assert 'isotropic ball-joint stiffness' in stage.warnings()


_TET_POINTS = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1)]


def _gmsh41_binary():
    import struct
    head = b'$MeshFormat\n4.1 1 8\n' + struct.pack('i', 1) + b'\n$EndMeshFormat\n'
    nodes = struct.pack('<QQQQiiiQ', 1, 4, 1, 4, 3, 1, 0, 4) + struct.pack('<4Q', 1, 2, 3, 4)
    nodes += b''.join(struct.pack('<3d', *p) for p in _TET_POINTS)
    elems = struct.pack('<QQQQiiiQ', 1, 1, 1, 1, 3, 1, 4, 1) + struct.pack('<5Q', 1, 1, 2, 3, 4)
    return head + b'$Nodes\n' + nodes + b'\n$EndNodes\n$Elements\n' + elems + b'\n$EndElements\n'


def _gmsh22_binary(tags):
    import struct
    head = b'$MeshFormat\n2.2 1 8\n' + struct.pack('i', 1) + b'\n$EndMeshFormat\n'
    nodes = b'4\n' + b''.join(struct.pack('<i3d', i + 1, *p) for i, p in enumerate(_TET_POINTS))
    if tags:  # gmsh: one block per element, with 2 tags
        elems = struct.pack('<3i', 4, 1, 2) + struct.pack('<7i', 1, 0, 0, 1, 2, 3, 4)
    else:     # fTetWild: one block, no tags
        elems = struct.pack('<3i', 4, 1, 0) + struct.pack('<5i', 1, 1, 2, 3, 4)
    return head + b'$Nodes\n' + nodes + b'\n$EndNodes\n$Elements\n1\n' + elems + b'\n$EndElements\n'


@pytest.mark.parametrize('data', [_gmsh41_binary(), _gmsh22_binary(True), _gmsh22_binary(False)],
                         ids=['4.1', '2.2-gmsh', '2.2-ftetwild'])
def test_binary_gmsh_flexcomp_matches_ascii(tmp_path, data):
    (tmp_path / 'ascii.msh').write_text(TET_GMSH)
    (tmp_path / 'binary.msh').write_bytes(data)
    def load(file):
        xml = (f'<mujoco><worldbody><body name="b" pos="0 0 1"><flexcomp name="f" type="gmsh" '
               f'file="{file}" radius=".001"/></body></worldbody></mujoco>')
        path = tmp_path / f'{file}.xml'
        path.write_text(xml)
        prim = lightusd.import_mjcf(path).prim_at('/World/Deformables/f')
        return prim.type_name, _points(prim).tolist(), [tuple(t) for t in prim['tetVertexIndices']]
    assert load('binary.msh') == load('ascii.msh')


def test_gmsh_and_legacy_msh_mesh_assets(tmp_path):
    import struct
    # .gmsh: MuJoCo 3.16's GMSH decoder, a tet mesh as its boundary surface.
    (tmp_path / 'tet.gmsh').write_bytes(_gmsh41_binary())
    # .msh: MuJoCo's legacy binary mesh (counts, vertices, faces).
    faces = [(0, 2, 1), (0, 1, 3), (0, 3, 2), (1, 2, 3)]
    (tmp_path / 'tet.msh').write_bytes(struct.pack('4i', 4, 0, 0, 4) +
                                       b''.join(struct.pack('3f', *p) for p in _TET_POINTS) +
                                       b''.join(struct.pack('3i', *f) for f in faces))
    for file in ('tet.gmsh', 'tet.msh'):
        xml = (f'<mujoco><asset><mesh name="m" file="{file}"/></asset><worldbody><body name="b">'
               '<freejoint/><geom type="mesh" mesh="m"/></body></worldbody></mujoco>')
        path = tmp_path / f'{file}.xml'
        path.write_text(xml)
        mesh = next(p for p in lightusd.import_mjcf(path)
                    if p.type_name == 'Mesh' and 'physics:collisionEnabled' in p.attributes)
        assert (len(mesh['points']), len(mesh['faceVertexIndices'])) == (4, 12), file
        mujoco = pytest.importorskip('mujoco')
        model = mujoco.MjModel.from_xml_path(str(path))
        assert (model.mesh_vertnum[0], model.mesh_facenum[0]) == (4, 4)
