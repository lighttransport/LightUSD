# SPDX-License-Identifier: Apache-2.0
import pytest

import lightusd

np = pytest.importorskip("numpy")


def build_stage():
    st = lightusd.Stage.create()
    st.up_axis = "Z"
    st.meters_per_unit = 1.0
    world = st.define_prim("/World", "Xform")
    grid = st.define_prim("/World/Grid", "Mesh")
    pts = np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], np.float32)
    grid.set("points", pts, type="point3f[]")
    grid.set("faceVertexCounts", np.array([4], np.int32))
    grid.set("faceVertexIndices", np.array([0, 1, 2, 3], np.int32))
    grid.set("displayColor", [(0.8, 0.2, 0.1)], type="color3f[]")
    grid.set("radius", 2.5, custom=True)
    grid.set("purpose", "render", uniform=True)
    grid.set("xformOpOrder", ["xformOp:translate"], type="token[]",
             uniform=True)
    grid.set("xformOp:translate", (0.0, 1.0, 0.0), time=0.0)
    grid.set("xformOp:translate", (0.0, 2.0, 0.0), time=24.0)
    st.define_prim("/World/Looks/Red", "Material")
    grid.add_relationship("material:binding", ["/World/Looks/Red"])
    world.set_metadata("kind", "assembly")
    st.set_default_prim("World")
    return st, pts


def verify(st2, pts):
    g = st2.prim_at("/World/Grid")
    assert np.allclose(np.asarray(g["points"]), pts)
    assert g["radius"] == 2.5
    assert g["purpose"] == "render"
    assert g["xformOpOrder"] == ("xformOp:translate",)
    assert g.attribute("purpose").is_uniform
    assert list(g.relationship("material:binding")) == ["/World/Looks/Red"]
    assert st2.prim_at("/World").kind == "assembly"
    attr = g.attribute("xformOp:translate")
    assert attr.has_timesamples and len(attr.timesamples) == 2
    assert st2.up_axis == "Z"
    assert st2.default_prim_path == "World"


def test_author_and_usda_roundtrip():
    st, pts = build_stage()
    # `custom` is a deprecated USDA qualifier the writer omits by default,
    # so assert it on the authoring stage only.
    assert st.prim_at("/World/Grid").attribute("radius").is_custom
    st2 = lightusd.loads(st.export_usda())
    verify(st2, pts)


def test_author_and_usdc_roundtrip():
    st, pts = build_stage()
    st2 = lightusd.load_bytes(st.export_usdc())
    verify(st2, pts)


def test_define_prim_variants_roundtrip():
    st = lightusd.Stage.create()
    w = st.define_prim("/World", "Xform")
    w.add_variant_set("lod")
    vs = w.variant_sets["lod"]
    vs.add_variant("high")
    vs.add_variant("low")
    vs.selection = "high"
    st2 = lightusd.loads(st.export_usda())
    vs2 = st2.prim_at("/World").variant_sets["lod"]
    assert set(vs2.names) == {"high", "low"}
    assert vs2.selection == "high"


def test_arcs_roundtrip():
    st = lightusd.Stage.create()
    w = st.define_prim("/World", "Xform")
    w.add_reference("./library.usda", "/Proto")
    w.add_inherit("/_class_base")
    usda = st.export_usda()
    assert "@./library.usda@</Proto>" in usda
    assert "</_class_base>" in usda


def test_value_types():
    st = lightusd.Stage.create()
    p = st.define_prim("/p", "Xform")
    p.set("a_bool", True)
    p.set("a_int", 7)
    p.set("a_float", 1.5)
    p.set("a_str", "hello", type="string")
    p.set("a_token", "tok")
    p.set("a_vec3", (1.0, 2.0, 3.0))
    p.set("a_ints", np.arange(5, dtype=np.int32))
    p.set("a_doubles", np.linspace(0, 1, 3))
    p.set("a_matrix", np.eye(4), type="matrix4d")
    st2 = lightusd.loads(st.export_usda())
    p2 = st2.prim_at("/p")
    assert p2["a_bool"] is True
    assert p2["a_int"] == 7
    assert p2["a_float"] == 1.5
    assert p2["a_str"] == "hello"
    assert p2["a_token"] == "tok"
    assert p2["a_vec3"] == (1.0, 2.0, 3.0)
    assert np.asarray(p2["a_ints"]).tolist() == [0, 1, 2, 3, 4]
    assert np.allclose(np.asarray(p2["a_doubles"]), [0, 0.5, 1])
    # matrix scalars read back as a flat row-major 16-tuple
    m = p2["a_matrix"]
    assert len(m) == 16
    assert np.allclose(np.asarray(m).reshape(4, 4), np.eye(4))


def test_attribute_set_and_block():
    st = lightusd.Stage.create()
    p = st.define_prim("/p", "Xform")
    p.set("radius", 1.0)
    attr = p.attribute("radius")
    attr.set(9.0)
    assert p["radius"] == 9.0
    attr.block()
    assert p["radius"] is lightusd.ValueBlock
    p.remove_property("radius")
    assert "radius" not in p


def test_remove_prim_and_staleness():
    st = lightusd.Stage.create()
    st.define_prim("/a/b/c", "Xform")
    b = st.prim_at("/a/b")
    st.define_prim("/a/d", "Xform")  # structural edit: handle self-heals
    assert b.name == "b"
    st.remove_prim("/a/b")
    with pytest.raises(lightusd.StaleHandleError):
        _ = b.name
    assert "/a/b/c" not in st
    with pytest.raises(KeyError):
        st.remove_prim("/nope")


def test_attribute_metadata_integer_range():
    st = lightusd.Stage.create()
    p = st.define_prim("/p", "Xform")
    p.set("count", 1)
    attr = p.attribute("count")
    attr.set_metadata("elementSize", 2**31 - 1)
    with pytest.raises(OverflowError):
        attr.set_metadata("elementSize", 2**31)
    with pytest.raises(OverflowError):
        attr.set_metadata("elementSize", -(2**31) - 1)


def test_metadata_and_sublayers():
    st = lightusd.Stage.create()
    st.set_metadata("doc", "my doc")
    st.set_metadata("startTimeCode", 1.0)
    st.set_metadata("endTimeCode", 100.0)
    st.add_sublayer("./base.usda")
    assert st.doc == "my doc"
    assert st.start_time == 1.0
    assert st.end_time == 100.0
    assert st.sublayers == ("./base.usda",)
    with pytest.raises(ValueError):
        st.set_metadata("upAxis", "Q")


def test_flattened(simple_stage):
    flat = simple_stage.flattened()
    assert flat.prim_at("/World/Quad")["radius"] == 2.5


def test_flatten_file(tmp_path, simple_stage):
    src = tmp_path / "in.usda"
    dst = tmp_path / "out.usdc"
    simple_stage.save(str(src))
    lightusd.flatten_file(str(src), str(dst))
    st = lightusd.load(dst)
    assert st.prim_at("/World/Quad")["radius"] == 2.5


def test_refcount_sanity(simple_stage):
    import sys

    q = simple_stage.prim_at("/World/Quad")
    base = sys.getrefcount(simple_stage)
    arrays = [q["points"] for _ in range(8)]
    del arrays
    prims = [p for p in simple_stage]
    del prims
    assert abs(sys.getrefcount(simple_stage) - base) <= 1


def test_variants_usdc_roundtrip():
    st = lightusd.Stage.create()
    w = st.define_prim("/World", "Xform")
    w.add_variant_set("lod")
    vs = w.variant_sets["lod"]
    vs.add_variant("high")
    vs.add_variant("low")
    vs.selection = "high"
    st2 = lightusd.load_bytes(st.export_usdc())
    vs2 = st2.prim_at("/World").variant_sets["lod"]
    assert set(vs2.names) == {"high", "low"}
    assert vs2.selection == "high"
    # second generation stays stable
    st3 = lightusd.load_bytes(st2.export_usdc())
    vs3 = st3.prim_at("/World").variant_sets["lod"]
    assert set(vs3.names) == {"high", "low"} and vs3.selection == "high"


VARIANT_SRC = '''#usda 1.0
def Xform "root" (
    variants = { string lod = "high" }
    prepend variantSets = ["lod"]
)
{
    variantSet "lod" = {
        "high" { float a = 1
                 def Mesh "Extra" { float b = 2 } }
        "low" { float a = 0 }
    }
}
'''


@pytest.mark.parametrize("ext", ["usda", "usdc"])
def test_variant_content_and_override(tmp_path, ext):
    st = lightusd.loads(VARIANT_SRC)
    fn = tmp_path / f"v.{ext}"
    st.save(str(fn))

    hi = lightusd.load(fn)  # composed with authored selection
    assert hi.prim_at("/root").get("a") == 1.0
    assert hi.prim_at("/root/Extra").get("b") == 2.0

    lo = lightusd.load(fn, variants={"lod": "low"})
    assert lo.prim_at("/root").get("a") == 0.0
    assert lo.get_prim_at("/root/Extra") is None


def test_variant_cross_format_chain(tmp_path):
    # usda -> usdc -> usda: names, selection and content all survive.
    st = lightusd.loads(VARIANT_SRC)
    mid = lightusd.load_bytes(st.export_usdc())
    final = lightusd.loads(mid.export_usda())
    fn = tmp_path / "chain.usda"
    final.save(str(fn))
    comp = lightusd.load(fn)
    assert comp.prim_at("/root").get("a") == 1.0
    assert comp.prim_at("/root/Extra").get("b") == 2.0


NESTED_VARIANT_SRC = '''#usda 1.0
def Xform "p" (variants = { string outer = "o1" } prepend variantSets = ["outer"]) {
    variantSet "outer" = {
        "o1" (variants = { string inner = "i2" }) {
            float a = 1
            variantSet "inner" = { "i1" { float b = 2 } "i2" { float b = 3 } }
        }
        "o2" { float a = 0 }
    }
}
'''


@pytest.mark.parametrize("ext", ["usda", "usdc"])
def test_nested_variants_compose(tmp_path, ext):
    st = lightusd.loads(NESTED_VARIANT_SRC)
    fn = tmp_path / f"n.{ext}"
    st.save(str(fn))
    comp = lightusd.load(fn)
    assert comp.prim_at("/p").get("a") == 1.0
    assert comp.prim_at("/p").get("b") == 3.0
    ov = lightusd.load(fn, variants={"inner": "i1"})
    assert ov.prim_at("/p").get("b") == 2.0


@pytest.mark.parametrize("ext", ["usda", "usdc"])
def test_variant_timesamples_and_connect(tmp_path, ext):
    st = lightusd.loads('''#usda 1.0
def Material "m" (variants = { string s = "a" } prepend variantSets = ["s"]) {
    variantSet "s" = {
        "a" { double h = 0
              double h.timeSamples = { 0: 1.0, 10: 11.0 }
              token outputs:surface.connect = </m/sh.outputs:out> }
        "b" { }
    }
    def Shader "sh" { token outputs:out }
}
''')
    fn = tmp_path / f"tc.{ext}"
    st.save(str(fn))
    comp = lightusd.load(fn)
    attr = comp.prim_at("/m").attribute("h")
    assert attr.has_timesamples and len(attr.timesamples) == 2
    assert abs(attr.get(time=5.0) - 6.0) < 1e-9
    surf = comp.prim_at("/m").attribute("outputs:surface")
    assert surf.connections == ("/m/sh.outputs:out",)


@pytest.mark.parametrize("ext", ["usda", "usdc"])
def test_variant_active_false_prunes(tmp_path, ext):
    st = lightusd.loads('''#usda 1.0
def Xform "p" (variants = { string s = "off" } prepend variantSets = ["s"]) {
    variantSet "s" = { "on" { float a = 1 } "off" (active = false) { } }
}
''')
    fn = tmp_path / f"act.{ext}"
    st.save(str(fn))
    assert lightusd.load(fn).prim_at("/p").active is False
    on = lightusd.load(fn, variants={"s": "on"})
    assert on.prim_at("/p").active is True and on.prim_at("/p").get("a") == 1.0


def test_variant_arrays_and_qualified_rel(tmp_path):
    st = lightusd.loads('''#usda 1.0
def Mesh "p" (variants = { string s = "a" } prepend variantSets = ["s"]) {
    variantSet "s" = {
        "a" { point3f[] points = [(0,0,0),(1,1,1)]
              custom rel myrel = </p>
              float after = 3 }
        "b" { }
    }
}
''')
    for reload in (lambda: lightusd.loads(st.export_usda()),
                   lambda: lightusd.load_bytes(st.export_usdc())):
        st2 = reload()
        assert set(st2.prim_at("/p").variant_sets["s"].names) == {"a", "b"}
    fn = tmp_path / "q.usdc"
    st.save(str(fn))
    comp = lightusd.load(fn)
    assert len(comp.prim_at("/p")["points"]) == 2
    assert comp.prim_at("/p").get("after") == 3.0
    assert list(comp.prim_at("/p").relationship("myrel")) == ["/p"]


def test_variant_delete_reference_arc(tmp_path):
    (tmp_path / "ref.usda").write_text(
        '#usda 1.0\ndef Sphere "Ball" { double radius = 42 }\n')
    scene = tmp_path / "s.usda"
    scene.write_text('''#usda 1.0
def Xform "p" (variants = { string s = "a" } prepend variantSets = ["s"]) {
    variantSet "s" = { "a" (delete references = @./ref.usda@</Ball>) { } "b" { } }
}
''')
    comp = lightusd.load(scene)
    assert comp.prim_at("/p").get("radius") is None


def test_multi_set_selection_crate_to_usda():
    st = lightusd.loads('''#usda 1.0
def Xform "p" (
    variants = { string shape = "a"  string color = "red" }
    prepend variantSets = ["shape", "color"]
) {
    variantSet "shape" = { "a" { float radius = 5 } "b" { } }
    variantSet "color" = { "red" { float tint = 1 } "blue" { } }
}
''')
    usda2 = lightusd.load_bytes(st.export_usdc()).export_usda()
    assert 'string shape = "a"' in usda2
    assert 'string color = "red"' in usda2


def test_dotted_variant_names_crate():
    st = lightusd.loads('''#usda 1.0
def Xform "p" (variants = { string lod = "hi_res-2.0" } prepend variantSets = ["lod"]) {
    variantSet "lod" = { "hi_res-2.0" { float a = 1 } "low.1" { float a = 0 } }
}
''')
    st2 = lightusd.load_bytes(st.export_usdc())
    vs = st2.prim_at("/p").variant_sets["lod"]
    assert set(vs.names) == {"hi_res-2.0", "low.1"}
    assert vs.selection == "hi_res-2.0"


def test_selection_only_prim_crate(tmp_path):
    # A prim that references an asset and only SELECTS a variant defined
    # there (no local variantSet) must keep the selection through USDC.
    (tmp_path / "asset.usda").write_text('''#usda 1.0
(
  defaultPrim = "Chair"
)
def Xform "Chair" (prepend variantSets = ["model"] variants = { string model = "A" }) {
    variantSet "model" = { "A" { float w = 1 } "B" { float w = 2 } }
}
''')
    (tmp_path / "scene.usda").write_text('''#usda 1.0
def Xform "c" (
    prepend references = @./asset.usda@</Chair>
    variants = { string model = "B" }
) { }
''')
    st = lightusd.load(tmp_path / "scene.usda", composed=False)
    fn = tmp_path / "scene.usdc"
    st.save(str(fn))
    comp = lightusd.load(fn)
    assert comp.prim_at("/c").get("w") == 2.0


def test_multi_set_flatten_pipeline(tmp_path):
    src = tmp_path / "m.usda"
    src.write_text('''#usda 1.0
def Xform "p" (
    variants = { string a = "x"  string b = "w" }
    prepend variantSets = ["a", "b"]
) {
    variantSet "a" = { "x" { float va = 1 } "y" { float va = 2 } }
    variantSet "b" = { "z" { float vb = 3 } "w" { float vb = 4 } }
}
''')
    dst = tmp_path / "m.usdc"
    lightusd.flatten_file(str(src), str(dst))
    flat = lightusd.load(dst)
    assert flat.prim_at("/p").get("va") == 1.0
    assert flat.prim_at("/p").get("vb") == 4.0


def test_token_array_metadata_and_usdz_assets(tmp_path):
    st = lightusd.Stage.create()
    prim = st.define_prim("/World/Body", "Xform")
    prim.set_metadata("apiSchemas", ["PhysicsRigidBodyAPI", "PhysicsCollisionAPI"])
    assert prim.metadata("apiSchemas") == ("PhysicsRigidBodyAPI", "PhysicsCollisionAPI")
    out = tmp_path / "scene.usdz"
    st.save_usdz(str(out), {"textures/albedo.bin": b"fixture"})
    import zipfile
    with zipfile.ZipFile(out) as archive:
        assert set(archive.namelist()) == {"root.usdc", "textures/albedo.bin"}
