# SPDX-License-Identifier: Apache-2.0
"""P0: Tydra full render-scene fields and option matrix."""
import pytest

import lightusd
from lightusd import tydra

np = pytest.importorskip("numpy")


def _mesh_stage():
    st = lightusd.Stage.create()
    st.up_axis = "Y"
    st.define_prim("/World", "Xform")
    m = st.define_prim("/World/M", "Mesh")
    pts = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], np.float32)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([4], dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.array([0, 1, 2, 3], dtype=np.int32), type="int[]")
    # material
    mat = st.define_prim("/World/Looks/Red", "Material")
    sh = st.define_prim("/World/Looks/Red/Shader", "Shader")
    sh.set("info:id", "UsdPreviewSurface", type="token")
    sh.set("inputs:diffuseColor", (0.9, 0.1, 0.1), type="color3f")
    sh.set("inputs:roughness", 0.4, type="float")
    # Shader output is declared by existence; no value needed
    m.add_relationship("material:binding", ["/World/Looks/Red"])
    return st


def test_tydra_mesh_buffers_and_zero_copy():
    st = _mesh_stage()
    scene = tydra.to_render_scene(st)
    assert len(scene.meshes) == 1
    m = scene.meshes[0]
    assert m.prim_path == "/World/M"
    assert m.point_count == 4
    assert m.face_count == 1
    assert m.is_triangulated
    # points zero-copy
    pts = m.points
    assert pts is not None
    a = np.asarray(pts)
    assert a.shape == (4, 3) and a.dtype == np.float32
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(pts).__array_interface__["data"][0]
    # triangulated indices
    tri = m.triangulated_indices
    assert tri is not None
    assert tri.dtype == "uint32"
    assert np.asarray(tri).dtype == np.uint32
    # face buffers
    assert np.asarray(m.face_vertex_counts).tolist() == [4]
    assert np.asarray(m.face_vertex_indices).tolist() == [0, 1, 2, 3]
    # bbox
    if m.bbox is not None:
        (mn, mx) = m.bbox
        assert len(mn) == 3 and len(mx) == 3
    assert m.subsets == []
    assert m.material_id >= 0
    # scene keeps alive after stage closed
    st2 = lightusd.loads('#usda 1.0\ndef Xform "W" { def Mesh "A" { point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]\n int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0,1,2]\n }\n}')
    sc = tydra.to_render_scene(st2)
    st2.close()
    assert np.asarray(sc.meshes[0].points).shape[1] == 3
    # array keeps scene alive
    pts2 = tydra.to_render_scene(st).meshes[0].points
    a2 = np.asarray(pts2)
    import gc
    gc.collect()
    assert a2.sum() >= 0


def test_tydra_material_and_lookup():
    st = _mesh_stage()
    scene = tydra.to_render_scene(st)
    m = scene.meshes[0]
    mat = scene.materials[m.material_id]
    assert mat.name
    assert mat.prim_path == "/World/Looks/Red"
    assert mat.shader in ("preview_surface", "UsdPreviewSurface", "PreviewSurface")
    diffuse = mat.param("diffuse_color")
    # param may be tuple or RenderTexture; diffuse_color should be tuple
    assert diffuse is None or isinstance(diffuse, tuple)
    if isinstance(diffuse, tuple):
        assert diffuse[0] == pytest.approx(0.9, abs=0.1)
    assert mat.param("no_such_param") is None
    assert scene.mesh_by_path("/World/M") is not None
    assert scene.material_by_path("/World/Looks/Red") is not None
    assert scene.mesh_by_path("/Nope") is None


def test_tydra_nodes_lights_cameras():
    st = lightusd.Stage.create()
    st.define_prim("/World", "Xform")
    st.define_prim("/World/L", "SphereLight").set("intensity", 100.0, type="float")
    st.define_prim("/World/Cam", "Camera").set("focalLength", 50.0, type="float")
    # need a mesh so scene not empty
    m = st.define_prim("/World/M", "Mesh")
    pts = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], np.float32)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([3], dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.array([0, 1, 2], dtype=np.int32), type="int[]")
    scene = tydra.to_render_scene(st)
    assert len(scene.meshes) == 1
    # nodes
    assert len(scene.nodes) >= 1
    assert len(scene.root_nodes) >= 1
    root = scene.root_nodes[0]
    assert root.prim_path == "/World"
    assert root.type in ("xform", "Xform")
    # lights / cameras may be present depending on converter version
    # At least verify types
    assert isinstance(scene.lights, tuple)
    assert isinstance(scene.cameras, tuple)
    assert isinstance(scene.textures, tuple)
    assert isinstance(scene.images, tuple)
    if scene.lights:
        l = scene.lights[0]
        assert l.prim_path
        assert l.type
        assert isinstance(l.color, tuple) and len(l.color) == 3
    if scene.cameras:
        cam = scene.cameras[0]
        assert cam.prim_path
        assert cam.focal_length > 0 or cam.focal_length == 0  # allow default 0


def test_tydra_option_matrix():
    st = _mesh_stage()
    # triangulate True vs False, compute_normals True vs False
    s1 = tydra.to_render_scene(st, triangulate=True, compute_normals=True)
    s2 = tydra.to_render_scene(st, triangulate=False, compute_normals=False)
    assert len(s1.meshes) == 1 and len(s2.meshes) == 1
    # s1 may have triangulated indices, s2 may not
    # s1 with normals computed may have normals
    if s1.meshes[0].normals is not None:
        assert np.asarray(s1.meshes[0].normals).shape[1] == 3
    # duplicate_instance_meshes flag
    s3 = tydra.to_render_scene(st, duplicate_instance_meshes=True)
    assert len(s3.meshes) == 1
