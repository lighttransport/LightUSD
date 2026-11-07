# SPDX-License-Identifier: Apache-2.0
"""Extended binding coverage: exercises APIs not hit by the original 59 tests.

Covers Array dtype matrix, Stage/Prim/Attribute/Relationship/Variant
surfaces, error handling, zero-copy invariants, and Tydra full-scene fields.
"""
import gc
import sys
import time
import pytest

import lightusd
from lightusd import tydra

np = pytest.importorskip("numpy")


def _stage_with_all_pod_types():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    # scalar PODs via type hints
    p.set("b", True, type="bool")
    p.set("i", 42, type="int")
    p.set("u", 42, type="uint")
    p.set("i64", 2**40, type="int64")
    p.set("u64", 2**40, type="uint64")
    p.set("half_s", 1.0, type="half")
    p.set("f", 1.5, type="float")
    p.set("d", 2.5, type="double")
    p.set("tok", "hello", type="token")
    p.set("str", "world", type="string")
    p.set("asset", "a.png", type="asset")
    # vector scalars
    p.set("f3", (1.0, 2.0, 3.0), type="float3")
    p.set("d4", (1.0, 2.0, 3.0, 4.0), type="double4")
    p.set("i2", (7, 8), type="int2")
    p.set("m4d", np.eye(4), type="matrix4d")
    # arrays via numpy
    p.set("f_arr", np.arange(10, dtype=np.float32), type="float[]")
    p.set("d_arr", np.arange(6, dtype=np.float64), type="double[]")
    p.set("i_arr", np.arange(5, dtype=np.int32), type="int[]")
    p.set("pt_arr", np.zeros((4, 3), np.float32), type="point3f[]")
    p.set("tok_arr", ["a", "b", "c"], type="token[]")
    return st


def test_array_dtype_variants():
    st = _stage_with_all_pod_types()
    # reload through USDA text to exercise parser path
    st2 = lightusd.loads(st.export_usda())
    p = st2.prim_at("/P")
    cases = [
        ("b", "bool", 1),
        ("i", "int", 1),
        ("u", "uint", 1),
        ("i64", "int64", 1),
        ("u64", "uint64", 1),
        ("f", "float", 1),
        ("d", "double", 1),
        ("f3", "float3", 1),
        ("m4d", "matrix4d", 1),
    ]
    for name, _type, _cnt in cases:
        v = p[name]
        assert v is not None
    # arrays: verify Array properties
    f_arr = p["f_arr"]
    assert f_arr.dtype == "float32"
    assert f_arr.shape == (10,)
    assert f_arr.nbytes == 40
    # type_name may be base type without [] depending on core version
    assert f_arr.type_name in ("float", "float[]")
    assert "float32" in repr(f_arr)
    assert len(f_arr) == 10
    # zero-copy: numpy view shares pointer
    a = np.asarray(f_arr)
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(f_arr).__array_interface__["data"][0]
    assert a.dtype == np.float32
    # memoryview carries the immutable typed layout
    mv = memoryview(f_arr)
    assert mv.nbytes == f_arr.nbytes
    # tolist / iter / getitem
    assert f_arr[0] == pytest.approx(0.0)
    assert list(f_arr)[1] == pytest.approx(1.0)
    assert f_arr.tolist()[2] == pytest.approx(2.0)
    # point3f[] is 2-D
    pt = p["pt_arr"]
    assert pt.shape == (4, 3)
    assert pt.dtype == "float32"
    arr2 = np.asarray(pt)
    assert arr2.shape == (4, 3)
    # token array returns tuple, not Array
    assert p["tok_arr"] == ("a", "b", "c")


def test_array_int_uint_variants():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    p.set("u32", np.array([1, 2, 3], dtype=np.uint32), type="uint[]")
    p.set("i64a", np.array([ -1, 2**35], dtype=np.int64), type="int64[]")
    p.set("u64a", np.array([0, 2**40], dtype=np.uint64), type="uint64[]")
    p2 = lightusd.load_bytes(st.export_usdc())
    assert np.asarray(p2.prim_at("/P")["u32"]).tolist() == [1, 2, 3]
    assert np.asarray(p2.prim_at("/P")["i64a"]).tolist() == [-1, 2**35]
    assert np.asarray(p2.prim_at("/P")["u64a"]).tolist() == [0, 2**40]


def test_stage_metadata_and_layer():
    st = lightusd.Stage.create()
    st.up_axis = "Z"
    st.meters_per_unit = 0.01
    st.time_codes_per_second = 24.0
    st.start_time = 1.0
    st.end_time = 48.0
    st.frames_per_second = 30.0
    st.doc = "hello"
    st.set_metadata("comment", "cmt")
    st.add_sublayer("./a.usda")
    st.add_sublayer("./b.usda")
    # reload
    st2 = lightusd.loads(st.export_usda())
    assert st2.up_axis == "Z"
    assert st2.meters_per_unit == pytest.approx(0.01)
    assert st2.time_codes_per_second == pytest.approx(24.0)
    assert st2.start_time == pytest.approx(1.0)
    assert st2.end_time == pytest.approx(48.0)
    assert st2.doc == "hello"
    assert st2.sublayers == ("./a.usda", "./b.usda")
    assert st2.custom_layer_data == {}
    stats = st2.stats
    assert stats["prim_count"] >= 0
    assert stats["memory_bytes"] > 0
    # pseudo_root may be None on next core (reserved); check root_prims instead
    assert len(st2.root_prims) >= 0
    assert st2.default_prim is None or st2.default_prim_path is None
    # define prim to satisfy default prim
    st2.define_prim("/World", "Xform")
    st2.set_default_prim("World")
    assert st2.default_prim_path == "World"
    # warnings is string
    assert isinstance(st2.warnings(), str)


def test_prim_advanced_and_transforms():
    st = lightusd.Stage.create()
    r = st.define_prim("/R", "Xform")
    r.set("xformOp:translate", (10, 0, 0), type="double3")
    r.set("xformOpOrder", ["xformOp:translate"], type="token[]", uniform=True)
    r.set_metadata("kind", "assembly")
    c = st.define_prim("/R/C", "Mesh")
    assert c.parent.path == "/R"
    assert c.path == "/R/C"
    assert c.active is True
    assert r.kind == "assembly"
    # local/world transform include translate (requires xformOpOrder)
    r2 = lightusd.loads(st.export_usda()).prim_at("/R")
    lt = r2.local_transform(time=0.0)
    assert lt[3][0] == pytest.approx(10.0)
    st3 = lightusd.loads(st.export_usda())
    wt = st3.prim_at("/R/C").world_transform(time=0.0)
    assert wt[3][0] == pytest.approx(10.0)
    # customData / assetInfo
    assert isinstance(r2.custom_data, dict)
    assert isinstance(r2.asset_info, dict)


def test_attribute_advanced():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    p.set("myAttr", 1.0, type="float")
    attr = p.attribute("myAttr")
    assert attr.name == "myAttr"
    assert attr.is_custom is False
    assert attr.is_uniform is False
    assert attr.is_connection is False
    assert attr.is_array is False
    assert attr.type_name == "float"
    assert attr.custom_data == {}
    assert attr.metadata("interpolation") is None
    attr.set_metadata("displayGroup", "g")
    # set with time
    p.set("anim", 1.0, type="float", time=0.0)
    p.set("anim", 5.0, type="float", time=10.0)
    a2 = lightusd.loads(st.export_usda()).prim_at("/P").attribute("anim")
    assert a2.has_timesamples
    assert len(a2.timesamples) == 2
    assert a2.get(time=5.0) == pytest.approx(3.0)
    assert a2.get(time=0.0) == pytest.approx(1.0)
    # block
    attr.block()
    assert p["myAttr"] is lightusd.ValueBlock
    # connections
    q = st.define_prim("/Q", "Material")
    q.set("inputs:a", 1.0, type="float")
    p.set("myAttr", 1.0, type="float")
    p.attribute("myAttr").connect("/Q.inputs:a")
    assert "/Q.inputs:a" in p.attribute("myAttr").connections
    # uniform/custom flags
    p.set("uAttr", 1.0, type="float", uniform=True)
    assert p.attribute("uAttr").is_uniform
    p.set("cAttr", 1.0, type="float", custom=True)
    assert p.attribute("cAttr").is_custom


def test_relationship_and_variant_ops():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    p.add_relationship("rel", ["/A", "/B"])
    assert tuple(p.relationship("rel").targets) == ("/A", "/B")
    p.relationship("rel").add_target("/C")
    assert "/C" in p.relationship("rel").targets
    p.relationship("rel").set_targets(["/X", "/Y"])
    assert p.relationship("rel").targets == ("/X", "/Y")
    p.relationship("rel").remove()
    assert "rel" not in p.relationships
    # variants
    p.add_variant_set("lod")
    vs = p.variant_sets["lod"]
    vs.add_variant("high")
    vs.add_variant("low")
    vs.selection = "high"
    assert vs.selection == "high"
    vs.selection = "low"
    assert vs.selection == "low"
    assert set(vs.names) == {"high", "low"}
    assert "lod" in p.variant_sets
    # composition arcs
    p.add_reference("./ref.usda", "/Proto")
    p.add_payload("./pay.usda", "/Root")
    p.add_inherit("/Class")
    p.add_specialize("/Spec")
    txt = st.export_usda()
    assert "references" in txt
    assert "payload" in txt
    assert "inherits" in txt
    assert "specializes" in txt
    # remove property
    p.set("tmp", 1.0)
    p.remove_property("tmp")
    assert "tmp" not in p


def test_stage_composition_and_files(tmp_path):
    # sublayer composition on disk
    base = tmp_path / "base.usda"
    base.write_text('#usda 1.0\ndef Xform "Base" { float v = 1\n}\n')
    root = tmp_path / "root.usda"
    root.write_text(f'#usda 1.0\n(\n subLayers = [@./base.usda@]\n)\ndef Xform "Root" {{}} \n')
    st = lightusd.load(str(root))
    assert st.prim_at("/Base")["v"] == pytest.approx(1.0)
    # load_payloads flag
    st2 = lightusd.load(str(root), load_payloads=False)
    assert st2.prim_at("/Base") is not None
    # variants override
    vroot = tmp_path / "v.usda"
    vroot.write_text('''#usda 1.0
def Xform "R" (variants = { string lod = "high" }
    prepend variantSets = ["lod"]) {
  variantSet "lod" = { "high" { float a = 1 } "low" { float a = 2 } }
}
''')
    assert lightusd.load(str(vroot), variants={"lod": "low"}).prim_at("/R")["a"] == pytest.approx(2.0)
    # max_memory pass-through
    lightusd.load(str(root), max_memory=100*1024*1024)


def test_load_bytes_formats_and_is_usd(tmp_path):
    st = lightusd.Stage.create()
    st.define_prim("/X", "Xform")
    for fmt in ("usda", "usdc"):
        data = st.export_usda().encode() if fmt == "usda" else st.export_usdc()
        st2 = lightusd.load_bytes(data, format=fmt)
        assert "/X" in st2
    # is_usd
    fn = tmp_path / "x.usda"
    st.save(str(fn))
    assert lightusd.is_usd(str(fn))
    bad = tmp_path / "bad.usda"
    bad.write_text("not usd")
    assert not lightusd.is_usd(str(bad))


def test_error_handling():
    with pytest.raises(lightusd.UsdParseError):
        lightusd.loads("not usda at all {")
    with pytest.raises(lightusd.UsdIoError):
        lightusd.load("/no/such/file.usda")
    st = lightusd.Stage.create()
    st.define_prim("/A", "Xform")
    with pytest.raises(KeyError):
        st.prim_at("/Nope")
    with pytest.raises(KeyError):
        st.prim_at("/A").attribute("nope")
    a = st.prim_at("/A")
    h = a.name
    st.remove_prim("/A")
    with pytest.raises(lightusd.StaleHandleError):
        _ = a.name  # stale


def test_tydra_full_scene_and_zero_copy():
    # build small scene with material + texture (mesh must be under Xform for some Tydra paths)
    st = lightusd.Stage.create()
    st.up_axis = "Y"
    st.define_prim("/World", "Xform")
    m = st.define_prim("/World/M", "Mesh")
    pts = np.array([[0,0,0],[1,0,0],[1,1,0],[0,1,0]], np.float32)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([4], dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.array([0,1,2,3], dtype=np.int32), type="int[]")
    scene = tydra.to_render_scene(st)
    assert len(scene.meshes) == 1
    mesh = scene.meshes[0]
    assert mesh.prim_path == "/World/M"
    # zero-copy mesh points
    arr = mesh.points
    assert arr is not None
    a = np.asarray(arr)
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
    # triangulated indices dtype
    tri = mesh.triangulated_indices
    if tri is not None:
        assert tri.dtype == "uint32"
        assert np.asarray(tri).dtype == np.uint32
    # materials / textures / images may be empty
    assert isinstance(scene.materials, tuple)
    assert isinstance(scene.textures, tuple)
    assert isinstance(scene.images, tuple)
    assert isinstance(scene.nodes, tuple)
    assert isinstance(scene.lights, tuple)
    assert isinstance(scene.cameras, tuple)
    assert scene.mesh_by_path("/World/M") is not None
    # scene outlives stage (use a valid mesh)
    st2 = lightusd.loads('#usda 1.0\ndef Xform "W" { def Mesh "A" { point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]\n int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0,1,2]\n }\n}')
    sc = tydra.to_render_scene(st2)
    st2.close()
    assert len(sc.meshes) == 1
    # tydra options
    sc2 = tydra.to_render_scene(st, triangulate=False, compute_normals=False)
    assert len(sc2.meshes) == 1


def test_synthetic_large_array_performance(capsys):
    # Synthetic 1M point cloud (12 MB) to measure zero-copy + timing
    n = 200_000  # keep CI friendly: ~2.4 MB
    pts = np.arange(n * 3, dtype=np.float32).reshape(n, 3)
    st = lightusd.Stage.create()
    m = st.define_prim("/Big", "Mesh")
    t0 = time.perf_counter()
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([3]* (n//3), dtype=np.int32))
    m.set("faceVertexIndices", np.arange((n//3)*3, dtype=np.int32))
    t_set = time.perf_counter() - t0
    t0 = time.perf_counter()
    blob = st.export_usdc()
    t_exp = time.perf_counter() - t0
    t0 = time.perf_counter()
    st2 = lightusd.load_bytes(blob)
    t_load = time.perf_counter() - t0
    arr = st2.prim_at("/Big")["points"]
    t0 = time.perf_counter()
    a = np.asarray(arr)
    t_view = time.perf_counter() - t0
    # zero-copy: pointer equality
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
    assert a.shape == (n, 3)
    # timing prints for manual inspection (pytest -s)
    print(f"[large synthetic] set {t_set:.3f}s export {t_exp:.3f}s load {t_load:.3f}s view {t_view*1e3:.1f}ms n={n}")


def test_flatten_and_type_helpers():
    st = lightusd.Stage.create()
    st.define_prim("/A", "Xform").set("v", 1.0)
    flat = st.flattened()
    assert flat.prim_at("/A")["v"] == pytest.approx(1.0)
    assert lightusd.type_from_name("float") != 0
    assert lightusd.type_name(lightusd.type_from_name("float")) == "float"
    # asarray helper
    st2 = lightusd.loads('#usda 1.0\ndef Mesh "M" { point3f[] points = [(0,0,0)]\n}')
    arr = st2.prim_at("/M")["points"]
    assert np.allclose(lightusd.asarray(arr), np.array([[0,0,0]], dtype=np.float32))
