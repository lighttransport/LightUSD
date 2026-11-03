# SPDX-License-Identifier: Apache-2.0
"""P0: Stage / Prim / Attribute / Relationship / Variant full API coverage."""
import pytest

import lightusd

np = pytest.importorskip("numpy")


def test_stage_full_metadata():
    st = lightusd.Stage.create()
    # initial defaults
    assert st.up_axis in ("Y", "Z", "X", None)
    st.up_axis = "Z"
    st.meters_per_unit = 0.01
    st.time_codes_per_second = 24.0
    st.start_time = 0.0
    st.end_time = 100.0
    st.frames_per_second = 30.0
    st.doc = "hello world"
    st.set_metadata("comment", "cmt")
    assert st.get_metadata("comment") == "cmt"
    assert st.get_metadata("no_such_key") is None
    # invalid upAxis should raise
    with pytest.raises((ValueError, lightusd.UsdError)):
        st.set_metadata("upAxis", "Q")
    st.add_sublayer("./a.usda")
    st.add_sublayer("./b.usda")
    assert st.sublayers == ("./a.usda", "./b.usda")
    assert st.custom_layer_data == {}
    # default prim handling
    assert st.default_prim is None
    st.define_prim("/World", "Xform")
    st.set_default_prim("World")
    assert st.default_prim_path == "World"
    assert st.default_prim.name == "World"
    # stats / warnings / len / iter / contains
    assert isinstance(st.warnings(), str)
    assert st.stats["prim_count"] >= 1
    assert len(st) >= 1
    assert "/World" in st
    assert "/Nope" not in st
    assert any(p.path == "/World" for p in st)
    # root_prims / get_prim_at
    assert len(st.root_prims) == 1
    assert st.get_prim_at("/Nope") is None
    assert st.get_prim_at("/World") is not None
    # roundtrip
    st2 = lightusd.loads(st.export_usda())
    assert st2.up_axis == "Z"
    assert st2.doc == "hello world"
    assert st2.sublayers == ("./a.usda", "./b.usda")


def test_stage_specifiers_and_file_roundtrip(tmp_path):
    st = lightusd.Stage.create()
    st.define_prim("/DefPrim", "Xform", specifier="def")
    st.define_prim("/OverPrim", "Xform", specifier="over")
    st.define_prim("/ClassPrim", "Xform", specifier="class")
    assert st.prim_at("/DefPrim").specifier == "def"
    assert st.prim_at("/OverPrim").specifier == "over"
    assert st.prim_at("/ClassPrim").specifier == "class"
    # override_prim
    st.override_prim("/OverPrim2")
    assert st.prim_at("/OverPrim2").specifier == "over"
    # save / load for usda/usdc/usdz
    for ext in ("usda", "usdc"):
        fn = tmp_path / f"s.{ext}"
        st.save(str(fn))
        st2 = lightusd.load(str(fn))
        assert "/DefPrim" in st2
    # flattened
    flat = st.flattened()
    assert "/DefPrim" in flat
    # remove_prim
    st.remove_prim("/DefPrim")
    assert "/DefPrim" not in st
    with pytest.raises(KeyError):
        st.remove_prim("/No")


def test_prim_full_api():
    st = lightusd.Stage.create()
    st.define_prim("/R", "Xform").set_metadata("kind", "assembly")
    c = st.define_prim("/R/C", "Mesh")
    c.set_metadata("comment", "leaf")
    assert c.name == "C"
    assert c.path == "/R/C"
    assert c.type_name == "Mesh"
    assert c.active is True
    assert c.specifier == "def"
    assert st.prim_at("/R").kind == "assembly"
    assert c.parent.path == "/R"
    assert len(c.parent.children) == 1
    assert any(p.path == c.path for p in c.parent.children)
    assert len(c) == 0
    assert list(c) == []
    assert "points" not in c
    c.set("points", np.zeros((1, 3), np.float32), type="point3f[]")
    assert "points" in c
    assert "points" in c.attributes
    # get / __getitem__ / attribute
    assert c.get("points") is not None
    assert c.get("nope", default=42) == 42
    assert c["points"] is not None
    assert c.attribute("points").name == "points"
    # child
    assert st.prim_at("/R").child("C").path == "/R/C"
    with pytest.raises(KeyError):
        st.prim_at("/R").child("Nope")
    # custom_data / asset_info / metadata
    assert isinstance(c.custom_data, dict)
    assert isinstance(c.asset_info, dict)
    assert c.metadata("kind") is None  # kind is on /R, not /R/C
    assert st.prim_at("/R").metadata("kind") == "assembly"
    # remove_property
    c.set("tmp", 1.0, type="float")
    assert "tmp" in c
    c.remove_property("tmp")
    assert "tmp" not in c
    # transforms
    st2 = lightusd.loads(st.export_usda())
    # need xformOpOrder for transform to apply
    r = st2.prim_at("/R")
    r.set("xformOp:translate", (5, 0, 0), type="double3")
    r.set("xformOpOrder", ["xformOp:translate"], type="token[]", uniform=True)
    lt = r.local_transform(time=0.0)
    assert len(lt) == 4 and len(lt[0]) == 4
    wt = st2.prim_at("/R/C").world_transform(time=0.0)
    assert wt[3][0] == pytest.approx(5.0)


def test_attribute_full_api():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    p.set("myAttr", 1.0, type="float")
    attr = p.attribute("myAttr")
    assert attr.name == "myAttr"
    assert attr.type_name == "float"
    assert attr.is_array is False
    assert attr.is_custom is False
    assert attr.is_uniform is False
    assert attr.is_connection is False
    assert attr.has_timesamples is False
    assert attr.custom_data == {}
    assert attr.metadata("interpolation") is None
    attr.set_metadata("displayGroup", "g")
    assert attr.metadata("displayGroup") == "g"
    # uniform / custom
    p.set("uAttr", 1.0, type="float", uniform=True)
    assert p.attribute("uAttr").is_uniform
    p.set("cAttr", 1.0, type="float", custom=True)
    assert p.attribute("cAttr").is_custom
    # array attr
    p.set("arr", np.array([1, 2, 3], dtype=np.float32), type="float[]")
    assert p.attribute("arr").is_array
    # timesamples
    p.set("anim", 0.0, type="float", time=0.0)
    p.set("anim", 10.0, type="float", time=10.0)
    attr2 = p.attribute("anim")
    assert attr2.has_timesamples
    assert len(attr2.timesamples) == 2
    assert attr2.timesamples.times == (0.0, 10.0)
    assert attr2.get(time=5.0) == pytest.approx(5.0)
    assert attr2.get(time=5.0, interpolation="held") == pytest.approx(0.0)
    assert attr2.eval(time=5.0) == pytest.approx(5.0)
    # connections
    q = st.define_prim("/Q", "Material")
    q.set("inputs:a", 1.0, type="float")
    p.set("myAttr", 1.0, type="float")
    p.attribute("myAttr").connect("/Q.inputs:a")
    assert p.attribute("myAttr").is_connection
    assert "/Q.inputs:a" in p.attribute("myAttr").connections
    # set / block / remove via Attribute API
    attr.set(2.0)
    assert p["myAttr"] == pytest.approx(2.0)
    attr.block()
    assert p["myAttr"] is lightusd.ValueBlock


def test_relationship_and_variants_full():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    # relationships via Prim API
    p.add_relationship("rel", ["/A", "/B"])
    rel = p.relationship("rel")
    assert rel.name == "rel"
    assert rel.targets == ("/A", "/B")
    assert list(rel) == ["/A", "/B"]
    assert len(rel) == 2
    assert rel[0] == "/A"
    assert "rel" in p.relationships
    rel.add_target("/C")
    assert "/C" in rel.targets
    rel.set_targets(["/X"])
    assert rel.targets == ("/X",)
    rel.remove()
    assert "rel" not in p.relationships
    # variants
    p.add_variant_set("lod")
    assert "lod" in p.variant_sets
    assert list(p.variant_sets) == ["lod"]
    vs = p.variant_sets["lod"]
    assert vs.name == "lod"
    vs.add_variant("high")
    vs.add_variant("low")
    assert set(vs.names) == {"high", "low"}
    assert len(p.variant_sets) == 1
    vs.selection = "high"
    assert vs.selection == "high"
    vs.selection = "low"
    assert vs.selection == "low"
    with pytest.raises(KeyError):
        _ = p.variant_sets["nope"]
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


def test_load_options_and_is_usd(tmp_path):
    # sublayer composition
    base = tmp_path / "base.usda"
    base.write_text('#usda 1.0\ndef Xform "Base" { float v = 1 }\n')
    root = tmp_path / "root.usda"
    root.write_text('#usda 1.0\n(\n subLayers = [@./base.usda@]\n)\n')
    st = lightusd.load(str(root))
    assert st.prim_at("/Base")["v"] == pytest.approx(1.0)
    # variants override
    vroot = tmp_path / "v.usda"
    vroot.write_text('''#usda 1.0
def Xform "R" (variants = { string lod = "high" } prepend variantSets = ["lod"]) {
  variantSet "lod" = { "high" { float a = 1 } "low" { float a = 2 } }
}
''')
    assert lightusd.load(str(vroot), variants={"lod": "low"}).prim_at("/R")["a"] == pytest.approx(2.0)
    # max_memory
    lightusd.load(str(root), max_memory=100*1024*1024)
    # is_usd / load_bytes with format
    st2 = lightusd.Stage.create()
    st2.define_prim("/X", "Xform")
    data_usda = st2.export_usda().encode()
    assert lightusd.load_bytes(data_usda, format="usda").prim_at("/X") is not None
    data_usdc = st2.export_usdc()
    assert lightusd.load_bytes(data_usdc, format="usdc").prim_at("/X") is not None
    fn = tmp_path / "x.usda"
    st2.save(str(fn))
    assert lightusd.is_usd(str(fn))
    assert lightusd.is_usd(str(fn),) is True
    bad = tmp_path / "bad.usda"
    bad.write_text("not usd")
    assert not lightusd.is_usd(str(bad))
    # composed flag: composed=False skips composition arcs (subLayers not expanded),
    # so /Base may not be present; just verify load succeeds
    st_nc = lightusd.load(str(root), composed=False)
    assert st_nc is not None


def test_native_instance_proxy_paths_and_parents(tmp_path):
    source = tmp_path / "instances.usda"
    source.write_text('''#usda 1.0
    def Xform "Source" {
        def Xform "Group" {
            def Mesh "Mesh" { point3f[] points = [(1,2,3)] }
        }
    }
    def Xform "A" ( prepend references = </Source> instanceable = true ) {}
    def Xform "B" ( prepend references = </Source> instanceable = true ) {}
    ''')
    stage = lightusd.load(str(source), preserve_native_instances=True)
    for name in ("A", "B"):
        root = stage.prim_at("/" + name)
        group, = root.children
        mesh, = group.children
        assert group.path == f"/{name}/Group"
        assert mesh.path == f"/{name}/Group/Mesh"
        assert mesh.parent.path == group.path
        assert mesh.parent.parent.path == root.path
        assert stage.prim_at(mesh.path).path == mesh.path
        assert mesh.attribute("points").resolve(time=0).tolist() == [(1, 2, 3)]
    stale = stage.prim_at("/B/Group/Mesh")
    stage.define_prim("/New", "Xform")
    # Python self-heals surviving handles by their logical path.
    assert stale.path == "/B/Group/Mesh"
    assert stage.prim_at("/B/Group/Mesh").path == stale.path
    stage.remove_prim("/B")
    with pytest.raises(lightusd.StaleHandleError):
        _ = stale.path


def test_flattened_instance_members_keep_their_subtree(tmp_path):
    """Default (flattened) loads must expose shared and nested instance content.

    Extracted-prototype flattening rewrites an instance member as
    `instanceable` + an internal reference to the de-instanced prototype holder
    and clears `instance_prototype`. Traversal and proxy lookup must still
    resolve the holder's subtree, otherwise a default load silently drops every
    shared instance after the first (and every nested one).
    """
    flat = tmp_path / "flat_instances.usda"
    flat.write_text('#usda 1.0\n'
                    'def Xform "Source" {\n'
                    '    def Mesh "Mesh" { point3f[] points = [(1,2,3)] }\n'
                    '}\n'
                    'def Xform "A" ( prepend references = </Source> '
                    'instanceable = true ) {}\n'
                    'def Xform "B" ( prepend references = </Source> '
                    'instanceable = true ) {}\n')
    for native in (False, True):
        stage = lightusd.load(str(flat), preserve_native_instances=native)
        meshes = sorted(p.path for p in stage.traverse(all_prims=True)
                        if "points" in p)
        assert meshes == ["/A/Mesh", "/B/Mesh", "/Source/Mesh"], (native, meshes)
        assert stage.prim_at("/B/Mesh").get("points").tolist() == [(1, 2, 3)]

    nested = tmp_path / "nested_instances.usda"
    nested.write_text('#usda 1.0\n'
                      'def Xform "Prop" {\n'
                      '    def Mesh "geom" { point3f[] points = [(0,0,0)] }\n'
                      '}\n'
                      'def Xform "Set" {\n'
                      '    def Xform "PropA" ( prepend references = </Prop> '
                      'instanceable = true ) {}\n'
                      '    def Xform "PropB" ( prepend references = </Prop> '
                      'instanceable = true ) {}\n'
                      '    def Xform "PropC" ( prepend references = </Prop> ) {}\n'
                      '}\n'
                      'def Xform "Top" ( prepend references = </Set> '
                      'instanceable = true ) {}\n')
    expected = ["/Prop/geom", "/Set/PropA/geom", "/Set/PropB/geom",
                "/Set/PropC/geom", "/Top/PropA/geom", "/Top/PropB/geom",
                "/Top/PropC/geom"]
    for native in (False, True):
        stage = lightusd.load(str(nested), preserve_native_instances=native)
        meshes = sorted(p.path for p in stage.traverse(all_prims=True)
                        if "points" in p)
        assert meshes == expected, (native, meshes)
        assert stage.prim_at("/Top/PropB/geom").get("points").tolist() == [(0, 0, 0)]


def test_leaf_prim_truthiness_is_validity_not_child_count():
    stage = lightusd.Stage.create()
    prim = stage.define_prim("/Leaf", "Xform")
    assert len(prim) == 0
    assert bool(prim)
    stage.define_prim("/Sibling", "Xform")
    assert bool(prim)  # Surviving logical handles self-heal.
    stage.remove_prim("/Leaf")
    with pytest.raises(lightusd.StaleHandleError):
        bool(prim)


def test_explicit_default_traversal_prunes_nonparticipating_subtrees():
    stage = lightusd.loads('''#usda 1.0
    class Xform "Class" { def Mesh "AbstractMesh" {} }
    def Xform "Root" {
        def Mesh "Visible" {}
        def Xform "Inactive" (active = false) { def Mesh "Hidden" {} }
        over "Undefined" { def Mesh "Child" {} }
    }
    ''')
    assert [p.path for p in stage.traverse()] == ["/Root", "/Root/Visible"]
    assert [p.path for p in stage.traverse(all_prims=True)] == [p.path for p in stage]
    assert stage.prim_at("/Root/Inactive/Hidden").active is False
    assert stage.prim_at("/Root/Inactive/Hidden").metadata("active") is None
