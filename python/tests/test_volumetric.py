# SPDX-License-Identifier: Apache-2.0
"""Level sets and tetrahedral meshes through tinyvdb: the Python API, MJCF
sdf sidecars, stage-level authoring, flexcomp stuffing and VDB inputs."""
import io
import math
import zipfile

import numpy as np
import pytest

import lightusd
from lightusd import volumetric as vol


def _sphere(nlat=24, nlon=48, r=1.0):
    pts = [(0, 0, r), (0, 0, -r)]
    for i in range(1, nlat):
        for j in range(nlon):
            th, ph = math.pi * i / nlat, 2 * math.pi * j / nlon
            pts.append((r * math.sin(th) * math.cos(ph), r * math.sin(th) * math.sin(ph),
                        r * math.cos(th)))
    ring = lambda i, j: 2 + (i - 1) * nlon + j % nlon
    tris = []
    for j in range(nlon):
        tris += [(0, ring(1, j), ring(1, j + 1)), (1, ring(nlat - 1, j + 1), ring(nlat - 1, j))]
    for i in range(1, nlat - 1):
        for j in range(nlon):
            tris += [(ring(i, j), ring(i + 1, j), ring(i + 1, j + 1)),
                     (ring(i, j), ring(i + 1, j + 1), ring(i, j + 1))]
    return np.array(pts, np.float32), np.array(tris, np.int32)


def _mesh_volume(points, tris):
    a, b, c = (points[tris[:, k]].astype(float) for k in range(3))
    return np.einsum('ij,ij->i', a, np.cross(b, c)).sum() / 6


def _tet_volumes(points, tets):
    a, b, c, d = (np.asarray(points, float)[np.asarray(tets)[:, k]] for k in range(4))
    return np.einsum('ij,ij->i', np.cross(b - a, c - a), d - a) / 6


def _closed(surface):
    edges = {}
    for f in np.asarray(surface):
        for k in range(3):
            e = (int(f[k]), int(f[(k + 1) % 3]))
            edges[e] = edges.get(e, 0) + 1
    return all(n == 1 and edges.get((b, a)) == 1 for (a, b), n in edges.items())


_CUBE_OBJ = '\n'.join(['v -.5 -.5 -.5', 'v .5 -.5 -.5', 'v .5 .5 -.5', 'v -.5 .5 -.5',
                       'v -.5 -.5 .5', 'v .5 -.5 .5', 'v .5 .5 .5', 'v -.5 .5 .5',
                       'f 1 3 2', 'f 1 4 3', 'f 5 6 7', 'f 5 7 8', 'f 1 2 6', 'f 1 6 5',
                       'f 2 3 7', 'f 2 7 6', 'f 3 4 8', 'f 3 8 7', 'f 4 1 5', 'f 4 5 8']) + '\n'


def test_mesh_level_set_samples_mesh_and_tets():
    pts, tris = _sphere()
    ls = vol.mesh_to_level_set(pts, tris, voxel_size=0.05)
    assert ls.voxel_size == pytest.approx(0.05) and ls.background == pytest.approx(0.15)
    assert ls.active_voxels > 10000
    q = np.array([[0, 0, 0], [1.1, 0, 0], [0.9, 0, 0], [0, 0, 0.95], [3, 0, 0]])
    v = ls.sample(q)
    # Interior tile, exact distances to the (inscribed) mesh, background.
    assert v[0] == pytest.approx(-0.15) and v[4] == pytest.approx(0.15)
    assert v[1] == pytest.approx(0.1, abs=0.01) and v[2] == pytest.approx(-0.1, abs=0.01)
    assert v[3] == pytest.approx(-0.05, abs=0.01)
    # Inside-out winding gives the same field.
    flipped = vol.mesh_to_level_set(pts, tris[:, ::-1].copy(), voxel_size=0.05)
    np.testing.assert_allclose(flipped.sample(q), v, atol=1e-6)
    surf_pts, surf_tris = ls.to_mesh()
    r = np.linalg.norm(surf_pts, axis=1)
    assert len(surf_tris) > 1000 and np.abs(r - 1).max() < 0.02
    tm = ls.to_tet_mesh(0.1)
    vols = _tet_volumes(tm.points, tm.tets)
    assert len(tm.tets) > 10000 and vols.min() > 0
    assert vols.sum() == pytest.approx(_mesh_volume(pts, tris), rel=0.02)
    assert _closed(tm.surface)
    # Round trip through .vdb bytes.
    again = vol.read_level_set(ls.vdb)
    np.testing.assert_array_equal(again.sample(q), v)


def test_level_set_errors():
    pts, tris = _sphere(8, 16)
    with pytest.raises(Exception):
        vol.mesh_to_level_set(pts, tris, voxel_size=0)
    with pytest.raises(Exception):
        vol.mesh_to_level_set(pts, tris, voxel_size=0.001, max_voxels=1000)
    with pytest.raises(Exception):
        vol.read_level_set(b'not a vdb')


def _sdf_mjcf(tmp_path):
    (tmp_path / 'cube.obj').write_text(_CUBE_OBJ)
    xml = ('<mujoco><asset><mesh name="cube" file="cube.obj"/></asset><worldbody>'
           '<body name="a" pos="0 0 1"><freejoint/><geom name="g" type="sdf" mesh="cube"/></body>'
           '<body name="b" pos="2 0 1"><freejoint/><geom name="h" type="sdf" mesh="cube"/></body>'
           '</worldbody></mujoco>')
    path = tmp_path / 'sdf.xml'
    path.write_text(xml)
    return path


def test_mjcf_sdf_geom_writes_level_set_sidecar(tmp_path):
    stage = lightusd.import_mjcf(_sdf_mjcf(tmp_path))
    assets = stage.generated_assets()
    assert list(assets) == ['sdf/cube.vdb']  # one sidecar shared by both geoms
    cols = [p for p in stage if p.type_name == 'Mesh' and 'lightusd:sdf' in p.relationships]
    assert len(cols) == 2
    for col in cols:
        assert 'NewtonSDFCollisionAPI' in (col.metadata('apiSchemas') or ())
        assert col['newton:sdfMaxResolution'] == 64
        assert col['newton:sdfNarrowBandOuter'] == pytest.approx(3 / 64, rel=1e-5)
        volume = stage.prim_at(col.relationship('lightusd:sdf').targets[0])
        assert volume.type_name == 'Volume' and volume['purpose'] == 'guide'
        field = stage.prim_at(volume.relationship('field:sdf').targets[0])
        assert field.type_name == 'OpenVDBAsset' and str(field['filePath']) == 'sdf/cube.vdb'
        assert field['fieldClass'] == 'levelSet'
    ls = vol.read_level_set(assets['sdf/cube.vdb'])
    # Inside the 3-voxel band (3/64) the values are exact box distances;
    # deeper inside they read the flood-filled interior value.
    v = ls.sample([[0, 0, 0], [0.5 + 2 / 64, 0, 0], [0.48, 0, 0]])
    assert v[0] == pytest.approx(-3 / 64) and v[1] == pytest.approx(2 / 64, abs=1e-4)
    assert v[2] == pytest.approx(-0.02, abs=1e-4)
    stage.save(str(tmp_path / 'out.usda'))
    assert (tmp_path / 'sdf' / 'cube.vdb').read_bytes() == assets['sdf/cube.vdb']
    stage.save(str(tmp_path / 'out.usdz'))
    assert 'sdf/cube.vdb' in zipfile.ZipFile(tmp_path / 'out.usdz').namelist()
    # openusd (no lightusd: data), sdf="off" and legacy author no sidecar.
    for kwargs in ({'profile': 'openusd'}, {'sdf': 'off'}, {'profile': 'legacy'}):
        other = lightusd.import_mjcf(_sdf_mjcf(tmp_path), **kwargs)
        assert 'sdf/cube.vdb' not in other.generated_assets()
        assert not [p for p in other if p.type_name == 'Volume']


_USDA_MESH = '''#usda 1.0
def Xform "World" {
  def Mesh "box" {
    double3 xformOp:translate = (1, 2, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    point3f[] points = [(-.5,-.5,-.5),(.5,-.5,-.5),(.5,.5,-.5),(-.5,.5,-.5),(-.5,-.5,.5),(.5,-.5,.5),(.5,.5,.5),(-.5,.5,.5)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0,3,2,1, 4,5,6,7, 0,1,5,4, 1,2,6,5, 2,3,7,6, 3,0,4,7]
  }
}
'''


def test_stage_add_mesh_sdf_and_volume_deformable(tmp_path):
    stage = lightusd.loads(_USDA_MESH)
    volume = vol.add_mesh_sdf(stage, '/World/box', resolution=32)
    assert volume.path == '/World/box_sdf' and volume['purpose'] == 'guide'
    assert tuple(volume['xformOp:translate']) == (1, 2, 3)
    box = stage.prim_at('/World/box')
    assert box.relationship('lightusd:sdf').targets == ('/World/box_sdf',)
    assert 'NewtonSDFCollisionAPI' in box.metadata('apiSchemas')
    assert 'sdf/box.vdb' in stage.generated_assets()
    # From the Mesh, and from the Volume's level set.
    for source in ('/World/box', '/World/box_sdf'):
        tets = vol.make_volume_deformable(stage, source, spacing=0.1, density=1000)
        assert tets.type_name == 'TetMesh'
        assert 'PhysicsVolumeDeformableSimAPI' in tets.metadata('apiSchemas')
        pts, t = np.asarray(tets['points']), np.asarray(tets['tetVertexIndices'])
        vols = _tet_volumes(pts, t)
        assert vols.min() > 0 and vols.sum() == pytest.approx(1.0, rel=0.03)
        assert sum(tets['physics:masses']) == pytest.approx(1000 * vols.sum(), rel=1e-4)
        assert _closed(np.asarray(tets['surfaceFaceVertexIndices']))
        material = stage.prim_at(tets.relationship('material:binding:physics').targets[0])
        assert 'PhysicsVolumeDeformableMaterialAPI' in material.metadata('apiSchemas')
        assert tuple(tets['xformOp:translate']) == (1, 2, 3)
    stage.save(str(tmp_path / 'stage.usda'))
    assert (tmp_path / 'sdf' / 'box.vdb').exists()


def test_flexcomp_stuffing_opt_in(tmp_path):
    (tmp_path / 'cube.obj').write_text(_CUBE_OBJ)
    xml = ('<mujoco><worldbody><flexcomp name="soft" type="mesh" file="cube.obj" dim="3" '
           'origin="0 0 0" radius=".001" mass="2"><elasticity young="1e4" poisson=".3"/>'
           '<pin id="0"/></flexcomp></worldbody></mujoco>')
    path = tmp_path / 'flex.xml'
    path.write_text(xml)
    fan = lightusd.import_mjcf(path).prim_at('/World/Deformables/soft')
    stuffed_stage = lightusd.import_mjcf(path, flex_tetrahedralize='stuffing', flex_tet_spacing=0.1)
    stuffed = stuffed_stage.prim_at('/World/Deformables/soft')
    assert len(fan['tetVertexIndices']) == 12  # MuJoCo: one tet per surface triangle
    vols = _tet_volumes(stuffed['points'], stuffed['tetVertexIndices'])
    assert len(vols) > 500 and vols.min() > 0 and vols.sum() == pytest.approx(1.0, rel=0.03)
    assert sum(stuffed['physics:masses']) == pytest.approx(2.0)
    warnings = stuffed_stage.warnings()
    assert 'tetrahedralized by isosurface stuffing' in warnings and 'pin is ignored' in warnings


def test_vdb_mesh_asset_and_flexcomp(tmp_path):
    pts, tris = _sphere()
    vol.mesh_to_level_set(pts * 0.2, tris, voxel_size=0.01).save(tmp_path / 'ball.vdb')
    xml = ('<mujoco><asset><mesh name="ball" file="ball.vdb"/></asset><worldbody>'
           '<body name="a"><freejoint/><geom type="mesh" mesh="ball" density="1000"/></body>'
           '<flexcomp name="soft" type="mesh" file="ball.vdb" dim="3" radius=".001" mass="1"/>'
           '</worldbody></mujoco>')
    path = tmp_path / 'vdb.xml'
    path.write_text(xml)
    stage = lightusd.import_mjcf(path)
    assert 'LightUSD extension' in stage.warnings()
    mesh = next(p for p in stage if p.type_name == 'Mesh' and 'physics:collisionEnabled' in p.attributes)
    r = np.linalg.norm(np.asarray(mesh['points']), axis=1)
    assert np.abs(r - 0.2).max() < 0.005
    tets = stage.prim_at('/World/Deformables/soft')
    vols = _tet_volumes(tets['points'], tets['tetVertexIndices'])
    assert vols.min() > 0 and vols.sum() == pytest.approx(4 / 3 * math.pi * 0.2 ** 3, rel=0.05)


def test_volumetric_cli(tmp_path, capsys):
    (tmp_path / 'box.usda').write_text(_USDA_MESH)
    out = tmp_path / 'soft.usda'
    assert vol.main([str(tmp_path / 'box.usda'), '-o', str(out), '--sdf', '/World/box',
                     '--deformable', '/World/box', '--spacing', '0.2']) == 0
    assert (tmp_path / 'sdf' / 'box.vdb').exists()
    stage = lightusd.load(str(out))
    assert stage.prim_at('/World/box_sdf').type_name == 'Volume'
    assert stage.prim_at('/World/box_tets').type_name == 'TetMesh'
    assert 'tets at /World/box_tets' in capsys.readouterr().out
