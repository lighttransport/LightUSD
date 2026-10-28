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


def test_level_set_handle():
    """A LevelSet is decoded once: bytes are written only on demand, read
    bytes are kept as given, and concurrent queries agree."""
    import threading
    pts, tris = _sphere(8, 16)
    ls = vol.mesh_to_level_set(pts, tris, voxel_size=0.1)
    assert ls._vdb is None  # no VDB written yet
    data = ls.vdb
    assert ls.vdb is data
    again = vol.read_level_set(bytearray(data))
    assert again.vdb == data
    q = np.random.default_rng(1).uniform(-1.2, 1.2, (2000, 3))
    want = ls.sample(q)
    np.testing.assert_array_equal(again.sample(q), want)
    got = [None] * 4

    def work(i):
        got[i] = again.sample(q)

    threads = [threading.Thread(target=work, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for g in got:
        np.testing.assert_array_equal(g, want)


def test_experimental_graded_level_set_tets():
    pts, tris = _sphere(12, 24)
    ls = vol.mesh_to_level_set(pts, tris, voxel_size=0.1)
    mesh = ls.to_tet_mesh(0.2, graded_coarse_spacing=0.6)
    volumes = _tet_volumes(mesh.points, mesh.tets)
    assert len(volumes) and volumes.min() > 0 and _closed(mesh.surface)
    assert volumes.sum() == pytest.approx(_mesh_volume(pts, tris), rel=0.12)
    quality = mesh.quality()
    assert quality["inverted_tets"] == 0
    assert quality["volume"] == pytest.approx(volumes.sum(), rel=1e-6)
    assert quality["min_tet_volume"] == pytest.approx(volumes.min(), rel=1e-6)
    assert 0 < quality["min_dihedral_degrees"] < quality["max_dihedral_degrees"] < 180
    again = ls.to_tet_mesh(0.2, graded_coarse_spacing=0.6)
    np.testing.assert_array_equal(mesh.points, again.points)
    np.testing.assert_array_equal(mesh.tets, again.tets)
    np.testing.assert_array_equal(mesh.surface, again.surface)
    guarded = ls.to_tet_mesh(0.2, graded_coarse_spacing=0.6, graded_surface_guard=True)
    assert len(guarded.tets) and _tet_volumes(guarded.points, guarded.tets).min() > 0
    assert _closed(guarded.surface)
    with pytest.raises(ValueError):
        ls.to_tet_mesh(0.2, graded_coarse_spacing=0.1)
    with pytest.raises(ValueError):
        ls.to_tet_mesh(0.2, graded_coarse_spacing=0.6, graded_growth=0)
    with pytest.raises(ValueError):
        ls.to_tet_mesh(0.2, graded_surface_guard=True)
    with pytest.raises(ValueError):
        ls.to_tet_mesh(0.2, graded_coarse_spacing=0.6, graded_surface_guard='yes')
    with pytest.raises(lightusd.UsdError, match="limit"):
        ls.to_tet_mesh(0.2, graded_coarse_spacing=0.6, max_background_tets=10)

    bad_tets = np.array(mesh.tets, copy=True)
    bad_tets[0, 0] = len(mesh.points)
    with pytest.raises(ValueError, match="tet index out of range"):
        vol.TetMesh(mesh.points, bad_tets, mesh.surface).quality()

    bad_points = np.array(mesh.points, copy=True)
    bad_points[mesh.tets[0, 0], 0] = np.nan
    with pytest.raises(ValueError, match="non-finite tetrahedron vertex"):
        vol.TetMesh(bad_points, mesh.tets, mesh.surface).quality()


def test_level_set_errors():
    pts, tris = _sphere(8, 16)
    with pytest.raises(Exception):
        vol.mesh_to_level_set(pts, tris, voxel_size=0)
    with pytest.raises(Exception):
        vol.mesh_to_level_set(pts, tris, voxel_size=0.001, max_voxels=1000)
    with pytest.raises(Exception):
        vol.read_level_set(b'not a vdb')
    # Settings are range-checked before use (no wraparound or float casts).
    for bad in (-1, 1 << 40):
        with pytest.raises(Exception, match='max_voxels'):
            vol.mesh_to_level_set(pts, tris, voxel_size=0.1, max_voxels=bad)
    with pytest.raises(Exception, match='threads'):
        vol.mesh_to_level_set(pts, tris, voxel_size=0.1, threads=1000)
    ls = vol.mesh_to_level_set(pts, tris, voxel_size=0.1)
    with pytest.raises(Exception, match='max_tets'):
        ls.to_tet_mesh(0.2, max_tets=-5)
    # Exact winding costs faces x voxels: refused up front past the limit.
    big, big_tris = _sphere(24, 48)
    with pytest.raises(Exception, match='exact winding'):
        vol.mesh_to_level_set(big, big_tris, voxel_size=0.01, exact_winding=True,
                              max_voxels=100_000_000)


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
    assert stage.import_report()['estimated_peak_memory_bytes'] >= (
        128 * ls.active_voxels + len(assets['sdf/cube.vdb']))
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


def test_mjcf_sdf_voxel_budget_uses_remaining_import_memory(tmp_path):
    path = _sdf_mjcf(tmp_path)
    # Parsing and retaining the source mesh leaves less than 100,000 voxels'
    # estimated workspace. The SDF builder must use that remainder instead of
    # restoring its old fixed minimum.
    stage = lightusd.import_mjcf(path, sdf_resolution=8, max_memory=128 * 1024)
    assert not stage.generated_assets()
    assert any(d['code'] == 'mjcf.sdf_level_set_failed'
               for d in stage.import_report()['diagnostics'])


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


_USDA_TWIN_MESHES = '''#usda 1.0
def Xform "A" {
  def Mesh "geom" {
    point3f[] points = [(-.5,-.5,-.5),(.5,-.5,-.5),(.5,.5,-.5),(-.5,.5,-.5),(-.5,-.5,.5),(.5,-.5,.5),(.5,.5,.5),(-.5,.5,.5)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0,3,2,1, 4,5,6,7, 0,1,5,4, 1,2,6,5, 2,3,7,6, 3,0,4,7]
  }
}
def Xform "B" {
  float3 xformOp:scale = (2, 2, 2)
  uniform token[] xformOpOrder = ["xformOp:scale"]
  def Mesh "geom" {
    point3f[] points = [(-.5,-.5,-.5),(.5,-.5,-.5),(.5,.5,-.5),(-.5,.5,-.5),(-.5,-.5,.5),(.5,-.5,.5),(.5,.5,.5),(-.5,.5,.5)]
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0,3,2,1, 4,5,6,7, 0,1,5,4, 1,2,6,5, 2,3,7,6, 3,0,4,7]
  }
}
'''


def test_add_mesh_sdf_unique_assets_and_scaled_band():
    stage = lightusd.loads(_USDA_TWIN_MESHES)
    a = vol.add_mesh_sdf(stage, '/A/geom', resolution=16)
    b = vol.add_mesh_sdf(stage, '/B/geom', resolution=16)
    assets = {str(stage.prim_at(v.path + '/sdf')['filePath']) for v in (a, b)}
    assert assets == {'sdf/geom.vdb', 'sdf/geom_2.vdb'}
    assert set(stage.generated_assets()) >= assets
    # Newton's narrow band is in shape units: only the unscaled mesh has it.
    assert 'newton:sdfNarrowBandInner' in stage.prim_at('/A/geom').attributes
    assert 'newton:sdfNarrowBandInner' not in stage.prim_at('/B/geom').attributes
    assert 'newton:sdfMaxResolution' in stage.prim_at('/B/geom').attributes


def test_volume_deformable_relative_path_needs_base_dir(tmp_path):
    pts, tris = _sphere(12, 24)
    vol.mesh_to_level_set(pts, tris, voxel_size=0.1).save(tmp_path / 'ball.vdb')
    usda = '''#usda 1.0
def Volume "V" {
  rel field:sdf = </V/sdf>
  def OpenVDBAsset "sdf" {
    asset filePath = @ball.vdb@
    token fieldClass = "levelSet"
  }
}
'''
    stage = lightusd.loads(usda)
    with pytest.raises(ValueError, match='base_dir'):
        vol.make_volume_deformable(stage, '/V', spacing=0.2)
    tets = vol.make_volume_deformable(stage, '/V', spacing=0.2, base_dir=str(tmp_path))
    assert len(tets['tetVertexIndices']) > 0


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


def test_flexcomp_graded_stuffing(tmp_path):
    (tmp_path / 'cube.obj').write_text(_CUBE_OBJ)
    path = tmp_path / 'flex.xml'
    path.write_text('<mujoco><worldbody><flexcomp name="soft" type="mesh" file="cube.obj" '
                    'dim="3" radius=".001" mass="2"/></worldbody></mujoco>')
    kwargs = dict(flex_tetrahedralize='stuffing', flex_tet_spacing=0.15,
                  flex_tet_coarse_spacing=0.45)
    stage = lightusd.import_mjcf(path, **kwargs)
    soft = stage.prim_at('/World/Deformables/soft')
    volumes = _tet_volumes(soft['points'], soft['tetVertexIndices'])
    assert len(volumes) and volumes.min() > 0
    assert volumes.sum() == pytest.approx(1.0, rel=0.08)
    assert sum(soft['physics:masses']) == pytest.approx(2.0)
    again = lightusd.import_mjcf(path, **kwargs).prim_at('/World/Deformables/soft')
    np.testing.assert_array_equal(soft['points'], again['points'])
    np.testing.assert_array_equal(soft['tetVertexIndices'], again['tetVertexIndices'])
    guarded = lightusd.import_mjcf(path, **kwargs, flex_tet_surface_guard=True).prim_at('/World/Deformables/soft')
    guarded_volumes = _tet_volumes(guarded['points'], guarded['tetVertexIndices'])
    assert len(guarded_volumes) and guarded_volumes.min() > 0
    assert guarded_volumes.sum() == pytest.approx(1.0, rel=0.08)
    with pytest.raises(ValueError, match='at least'):
        lightusd.import_mjcf(path, flex_tet_spacing=0.2, flex_tet_coarse_spacing=0.1)
    with pytest.raises(ValueError, match='cpu'):
        lightusd.import_mjcf(path, flex_tet_coarse_spacing=0.4, volumetric_device='auto')
    with pytest.raises(ValueError, match='requires'):
        lightusd.import_mjcf(path, flex_tet_surface_guard=True)
    with pytest.raises(lightusd.UsdError, match='flex_tetrahedralize=stuffing'):
        lightusd.import_mjcf(path, flex_tet_coarse_spacing=0.4)
    with pytest.raises(lightusd.UsdError, match='resolved fine spacing'):
        lightusd.import_mjcf(path, flex_tetrahedralize='stuffing', flex_tet_coarse_spacing=0.01)
    # A small import budget must constrain candidate voxels, not silently
    # restore the old 100,000-voxel minimum.
    with pytest.raises(lightusd.UsdError, match='level set: .*budget exceeded'):
        lightusd.import_mjcf(path, flex_tetrahedralize='stuffing',
                             flex_tet_spacing=0.2, max_memory=800_000)


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
    graded_stage = lightusd.import_mjcf(path, flex_tet_spacing=0.04,
                                      flex_tet_coarse_spacing=0.12)
    assert 'level-set band is too narrow' in graded_stage.warnings()
    graded = graded_stage.prim_at('/World/Deformables/soft')
    vols = _tet_volumes(graded['points'], graded['tetVertexIndices'])
    assert len(vols) and vols.min() > 0
    assert vols.sum() == pytest.approx(4 / 3 * math.pi * 0.2 ** 3, rel=0.10)


def test_vdb_flexcomp_decode_respects_import_memory(tmp_path):
    pts, tris = _sphere(12, 24)
    path = tmp_path / 'sphere.vdb'
    vol.mesh_to_level_set(pts, tris, voxel_size=0.05).save(path)
    scene = tmp_path / 'scene.xml'
    scene.write_text('<mujoco><worldbody><flexcomp name="soft" type="mesh" '
                     'file="sphere.vdb" dim="3"/></worldbody></mujoco>')
    # The compressed file fits, but its decoded tree does not fit after the
    # importer has accounted for the XML and asset bytes.
    assert path.stat().st_size < 400_000
    with pytest.raises(lightusd.UsdError, match='VDB read: memory budget exceeded'):
        lightusd.import_mjcf(scene, max_memory=800_000, flex_tet_spacing=0.2)
    stage = lightusd.import_mjcf(scene, max_memory=16_000_000, flex_tet_spacing=0.2)
    mesh = stage.prim_at('/World/Deformables/soft')
    volumes = _tet_volumes(mesh['points'], mesh['tetVertexIndices'])
    assert len(volumes) and volumes.min() > 0
    assert volumes.sum() == pytest.approx(_mesh_volume(pts, tris), rel=0.06)


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


_GPU = vol.device_available("auto")


def test_device_names():
    assert vol.device_available("cpu") == "cpu"
    assert _GPU in ("cpu", "cuda", "hip")
    assert vol.device_available("bogus") is None
    points, tris = _sphere(8, 16)
    with pytest.raises(ValueError):
        vol.mesh_to_level_set(points, tris, resolution=16, device="bogus")
    for name in ("cuda", "hip"):
        if vol.device_available(name) is None:  # explicit and unavailable: an error
            with pytest.raises(lightusd.UsdError):
                vol.mesh_to_level_set(points, tris, resolution=16, device=name)
    # "auto" always runs, on the CPU when there is no device.
    assert vol.mesh_to_level_set(points, tris, resolution=16, device="auto").active_voxels > 0


@pytest.mark.skipif(_GPU not in ("cuda", "hip"), reason="no CUDA/HIP device in this build")
def test_gpu_level_set_and_tets_match_cpu():
    points, tris = _sphere(32, 64)
    cpu = vol.mesh_to_level_set(points, tris, resolution=48)
    gpu = vol.mesh_to_level_set(points, tris, resolution=48, device=_GPU)
    assert gpu.active_voxels == cpu.active_voxels
    h = cpu.voxel_size
    grid = np.stack(np.meshgrid(*[np.arange(-1.2, 1.2, h)] * 3, indexing="ij"), -1).reshape(-1, 3)
    a, b = np.asarray(cpu.sample(grid)), np.asarray(gpu.sample(grid))
    assert np.max(np.abs(np.abs(a) - np.abs(b))) <= 1e-6 * h
    # Signs agree off the surface (on it, |d| is zero to rounding).
    off = np.minimum(np.abs(a), np.abs(b)) > 1e-5 * h
    assert np.all(np.sign(a[off]) == np.sign(b[off]))
    tc, tg = cpu.to_tet_mesh(4 * h), cpu.to_tet_mesh(4 * h, device=_GPU)
    assert np.array_equal(np.asarray(tc.points), np.asarray(tg.points))
    assert np.array_equal(np.asarray(tc.tets), np.asarray(tg.tets))
    assert np.array_equal(np.asarray(tc.surface), np.asarray(tg.surface))


def test_mjcf_volumetric_device_option(tmp_path):
    xml = _sdf_mjcf(tmp_path)
    cpu = lightusd.import_mjcf(str(xml))
    auto = lightusd.import_mjcf(str(xml), volumetric_device="auto")
    assert sorted(auto.generated_assets()) == sorted(cpu.generated_assets())
    assert "volumetric_device_unavailable" not in auto.warnings()
    with pytest.raises(ValueError):
        lightusd.import_mjcf(str(xml), volumetric_device="tpu")
    for name in ("cuda", "hip"):
        stage = lightusd.import_mjcf(str(xml), volumetric_device=name)
        assert sorted(stage.generated_assets()) == sorted(cpu.generated_assets())
        if vol.device_available(name) is None:  # importers fall back to the CPU
            assert "using the CPU" in stage.warnings()
            assert stage.generated_assets() == cpu.generated_assets()


def test_render_scene_volume_nanovdb_and_sample(tmp_path):
    """RenderScene exposes the sparse grid as NanoVDB and samples world points."""
    from lightusd import tydra
    pts, tris = _sphere(12, 24)
    level_set = vol.mesh_to_level_set(pts, tris, resolution=24)
    vdb = tmp_path / "sdf.vdb"
    level_set.save(vdb)

    stage = lightusd.loads(f'''#usda 1.0
def Volume "V"
{{
    rel field:sdf = </V/sdf>
    def OpenVDBAsset "sdf"
    {{
        asset filePath = @{vdb.as_posix()}@
        token fieldName = "sdf"
        token fieldClass = "levelSet"
    }}
}}
''')
    scene = tydra.to_render_scene(stage)
    volumes = scene.volumes()
    assert len(volumes) == 1 and volumes[0]["prim_path"] == "/V"
    assert [f["binding"] for f in volumes[0]["fields"]] == ["sdf"]
    field = volumes[0]["fields"][0]
    assert field["value_type"] == "float" and field["components"] == 1
    assert field["level_set"] is True

    nanovdb = scene.volume_nanovdb(0, 0)
    assert nanovdb is not None
    assert nanovdb["grid_type"] == 1 and nanovdb["components"] == 1
    assert nanovdb["value_type"] == "float" and nanovdb["grid_class"] == "levelSet"
    # NanoVDB's own GridHandle writes the grid magic "NanoVDB1".
    assert nanovdb["data"][:8] == b"NanoVDB1" and len(nanovdb["data"]) > 736
    # Cached: a second call returns the same bytes.
    assert scene.volume_nanovdb(0, 0)["data"] == nanovdb["data"]

    # Sample inside the band: trilinear agrees with the level-set reader (the
    # same tinyvdb sampler the file API uses); nearest picks stored values.
    rng = np.random.default_rng(7)
    direction = rng.normal(size=(64, 3))
    direction /= np.linalg.norm(direction, axis=1, keepdims=True)
    radius = rng.uniform(0.2, 1.1, size=(64, 1))
    points = direction * radius  # the sphere was meshed with radius 1
    nearest = scene.volume_sample(0, 0, points)
    trilinear = scene.volume_sample(0, 0, points, trilinear=True)
    assert tuple(nearest.shape) == (64,) and tuple(trilinear.shape) == (64,)
    reference = np.asarray(level_set.sample(points))
    assert np.allclose(np.asarray(trilinear).reshape(-1), reference, atol=1e-6)

    # Acceptance: flat coordinates and empty input.
    flat = scene.volume_sample(0, 0, np.asarray(points, np.float64).reshape(-1))
    assert np.array_equal(np.asarray(flat), np.asarray(nearest))
    assert scene.volume_sample(0, 0, []) is None
    with pytest.raises(ValueError):
        scene.volume_sample(0, 0, [1.0, 2.0])
    with pytest.raises(ValueError):
        scene.volume_sample(0, 0, [(1.0, 2.0)])


def test_mjcf_sdf_releases_temporary_grid_charges_between_assets(tmp_path):
    source = _sdf_mjcf(tmp_path)
    single = lightusd.import_mjcf(source, sdf_resolution=8)
    budget = single.import_report()['estimated_peak_memory_bytes'] + 256 * 1024
    # Separate mesh records must each generate a sidecar. Retaining each
    # construction peak would incorrectly prevent the later assets fitting.
    source.write_text('''<mujoco><asset>
      <mesh name="a" file="cube.obj"/><mesh name="b" file="cube.obj"/>
      <mesh name="c" file="cube.obj"/></asset><worldbody>
      <body><freejoint/><geom type="sdf" mesh="a"/></body>
      <body pos="2 0 0"><freejoint/><geom type="sdf" mesh="b"/></body>
      <body pos="4 0 0"><freejoint/><geom type="sdf" mesh="c"/></body>
      </worldbody></mujoco>''')
    stage = lightusd.import_mjcf(source, sdf_resolution=8, max_memory=budget)
    assert set(stage.generated_assets()) == {'sdf/a.vdb', 'sdf/b.vdb', 'sdf/c.vdb'}
    assert stage.import_report()['estimated_peak_memory_bytes'] <= budget
    assert not [d for d in stage.import_report()['diagnostics']
                if d['code'] == 'mjcf.sdf_level_set_failed']
    # A failed optional generation must not poison a later import's budget.
    small = lightusd.import_mjcf(source, sdf_resolution=8, max_memory=128 * 1024)
    assert not small.generated_assets()
    retry = lightusd.import_mjcf(source, sdf_resolution=8, max_memory=budget)
    assert retry.generated_assets() == stage.generated_assets()
