# SPDX-License-Identifier: Apache-2.0
"""USD -> OBJ/MTL export (lightusd.objexport) round trips through import."""
import numpy as np
import pytest

import lightusd
from lightusd.objexport import export_obj

SOURCE = b'''mtllib m.mtl
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 2 0 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
vw 0 0 1
vw 1 0 0.5 1 0.5
vw 2 1 1
vw 3 0 1
vw 4 1 1
usemtl red
f 1/1/1 2/2/1 3/3/1 4/4/1
usemtl blue
f 2/1/1 5/2/1 3/3/1
'''
MTL = b'''newmtl red
Kd 1 0 0
d 0.5
newmtl blue
Kd 0 0 1
map_Kd tex.png
'''


def _mesh(stage):
    return next(p for p in stage if p.type_name == 'Mesh')


def _corners(mesh, name):
    values = np.asarray(mesh[name], dtype=float).reshape(len(mesh[name]), -1)
    if f'{name}:indices' in mesh.attributes:
        values = values[np.asarray(mesh[f'{name}:indices'])]
    return values


def test_round_trip_geometry_materials_and_skin(tmp_path):
    (tmp_path / 'in.obj').write_bytes(SOURCE)
    (tmp_path / 'm.mtl').write_bytes(MTL)
    (tmp_path / 'tex.png').write_bytes(b'')
    source = lightusd.import_obj(tmp_path / 'in.obj')
    result = export_obj(source, tmp_path / 'out.obj')
    assert result.meshes == 1 and result.faces == 2 and result.materials == 2
    assert result.skinned_vertices == result.vertices == len(_mesh(source)['points'])
    text = (tmp_path / 'out.obj').read_text()
    assert 'usemtl red' in text and 'usemtl blue' in text and 'vw 1 0 0.5 1 0.5' in text
    mtl = (tmp_path / 'out.mtl').read_text()
    assert 'Kd 1 0 0' in mtl and 'd 0.5' in mtl and 'map_Kd' in mtl
    again = lightusd.import_obj(tmp_path / 'out.obj')
    a, b = _mesh(source), _mesh(again)
    assert list(b['faceVertexCounts']) == [4, 3]
    pa = np.asarray(a['points'])[np.asarray(a['faceVertexIndices'])]
    pb = np.asarray(b['points'])[np.asarray(b['faceVertexIndices'])]
    np.testing.assert_allclose(pa, pb)
    # Per-corner UVs survive (points may be re-split differently).
    st_a = _corners(a, 'primvars:st')[np.asarray(a['faceVertexIndices'])]
    st_b = _corners(b, 'primvars:st')[np.asarray(b['faceVertexIndices'])]
    np.testing.assert_allclose(st_a, st_b)
    assert list(b['primvars:skel:jointWeights']) == pytest.approx(list(a['primvars:skel:jointWeights']))


def test_world_space_and_triangulate(tmp_path):
    stage = lightusd.loads('''#usda 1.0
def Xform "R" (prepend apiSchemas = []) {
    double3 xformOp:translate = (10, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    def Mesh "M" {
        point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        normal3f[] normals = [(0,0,1)] (interpolation = "constant")
    }
}
''')
    export_obj(stage, tmp_path / 'w.obj', triangulate=True, materials=False)
    text = (tmp_path / 'w.obj').read_text()
    assert 'v 10 0 0' in text and 'mtllib' not in text
    assert sum(1 for line in text.splitlines() if line.startswith('f ')) == 2
    local = export_obj(stage, tmp_path / 'l.obj', world_space=False)
    assert 'v 0 0 0' in (tmp_path / 'l.obj').read_text() and local.faces == 1
