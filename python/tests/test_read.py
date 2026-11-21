# SPDX-License-Identifier: Apache-2.0
import pytest

import lightusd

np = pytest.importorskip("numpy")


def test_traversal(simple_stage):
    paths = [p.path for p in simple_stage]
    assert paths == [
        "/World",
        "/World/Quad",
        "/World/Looks",
        "/World/Looks/Red",
        "/World/Looks/Red/Shader",
    ]
    assert "/World/Quad" in simple_stage
    assert "/Nope" not in simple_stage
    world = simple_stage.prim_at("/World")
    assert [c.name for c in world] == ["Quad", "Looks"]
    assert len(world) == 2
    assert world.children[0].type_name == "Mesh"
    assert simple_stage.prim_at("/World/Quad").parent.path == "/World"
    assert world.child("Quad").path == "/World/Quad"
    with pytest.raises(KeyError):
        world.child("Nope")
    with pytest.raises(KeyError):
        simple_stage.prim_at("/Nope")
    assert simple_stage.get_prim_at("/Nope") is None
    meshes = simple_stage.prims_of_type("Mesh")
    assert len(meshes) == 1 and meshes[0].path == "/World/Quad"
    roots = simple_stage.root_prims
    assert len(roots) == 1 and roots[0].name == "World"


def test_prim_basics(simple_stage):
    q = simple_stage.prim_at("/World/Quad")
    assert q.name == "Quad"
    assert q.type_name == "Mesh"
    assert q.specifier == "def"
    assert q.active is True
    assert simple_stage.prim_at("/World").kind == "assembly"
    assert "Prim(" in repr(q)
    assert "points" in q
    assert "nope" not in q
    assert "points" in q.attributes
    assert "material:binding" in q.relationships


def test_attribute_values_zero_copy(simple_stage):
    q = simple_stage.prim_at("/World/Quad")
    pts = q["points"]
    assert pts.shape == (4, 3)
    assert pts.dtype == "float32"
    a = np.asarray(pts)
    assert a.shape == (4, 3) and a.dtype == np.float32
    # zero-copy: same base pointer
    assert np.asarray(pts).__array_interface__["data"][0] == np.asarray(a).__array_interface__["data"][0]
    assert pts[1] == (1.0, 0.0, 0.0)
    assert pts.tolist()[2] == (1.0, 1.0, 0.0)
    mv = memoryview(pts)
    assert mv.nbytes == pts.nbytes

    assert q["radius"] == 2.5
    assert q["purpose"] == "render"
    counts = q["faceVertexCounts"]
    assert np.asarray(counts).tolist() == [4]
    assert q.get("nope", default=42) == 42
    with pytest.raises(KeyError):
        q["nope"]


def test_attribute_object(simple_stage):
    q = simple_stage.prim_at("/World/Quad")
    attr = q.attribute("points")
    assert attr.name == "points"
    assert attr.type_name.startswith("point3f")
    assert attr.is_array
    assert not attr.has_timesamples
    st_attr = q.attribute("primvars:st")
    assert st_attr.interpolation == "vertex"
    with pytest.raises(KeyError):
        q.attribute("nope")


def test_timesamples(simple_stage):
    q = simple_stage.prim_at("/World/Quad")
    attr = q.attribute("xformOp:translate")
    assert attr.has_timesamples
    ts = attr.timesamples
    assert len(ts) == 2
    assert ts.times == (0.0, 24.0)
    t0, v0 = ts[0]
    assert t0 == 0.0 and v0 == (1.0, 0.0, 0.0)
    t1, v1 = ts[-1]
    assert t1 == 24.0 and v1 == (5.0, 0.0, 0.0)
    assert abs(attr.get(time=12.0)[0] - 3.0) < 1e-9
    assert attr.get(time=12.0, interpolation="held") == (1.0, 0.0, 0.0)


def test_relationships(simple_stage):
    q = simple_stage.prim_at("/World/Quad")
    rel = q.relationship("material:binding")
    assert rel.name == "material:binding"
    assert rel.targets == ("/World/Looks/Red",)
    assert list(rel) == ["/World/Looks/Red"]
    assert len(rel) == 1
    with pytest.raises(KeyError):
        q.relationship("nope")


def test_connections(simple_stage):
    mat = simple_stage.prim_at("/World/Looks/Red")
    attr = mat.attribute("outputs:surface")
    assert attr.connections == ("/World/Looks/Red/Shader.outputs:surface",)


def test_transforms(simple_stage):
    world = simple_stage.prim_at("/World")
    lt = world.local_transform()
    assert lt[3][:3] == (1.0, 2.0, 3.0)
    quad = simple_stage.prim_at("/World/Quad")
    wt = quad.world_transform(time=0.0)
    # The Quad's translate op is not listed in an xformOpOrder, so only the
    # parent transform applies (correct USD semantics).
    assert wt[3][:3] == (1.0, 2.0, 3.0)


def test_variants():
    st = lightusd.loads('''#usda 1.0
def Xform "root" (
    variants = { string lod = "high" }
    prepend variantSets = ["lod"]
)
{
    variantSet "lod" = {
        "high" { float a = 1 }
        "low" { float a = 0 }
    }
}
''')
    root = st.prim_at("/root")
    vsets = root.variant_sets
    assert len(vsets) == 1
    assert "lod" in vsets
    assert list(vsets) == ["lod"]
    vs = vsets["lod"]
    assert set(vs.names) == {"high", "low"}
    assert vs.selection == "high"
    with pytest.raises(KeyError):
        vsets["nope"]


def test_custom_data():
    st = lightusd.loads('''#usda 1.0
def Xform "a" (
    customData = {
        string owner = "me"
        int version = 3
    }
)
{
}
''')
    cd = st.prim_at("/a").custom_data
    assert cd.get("owner") == "me"
    assert cd.get("version") == 3


def test_stage_stats(simple_stage):
    stats = simple_stage.stats
    assert stats["prim_count"] == 5
    assert stats["memory_bytes"] > 0
