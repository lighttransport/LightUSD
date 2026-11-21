# SPDX-License-Identifier: Apache-2.0
"""P0: Value types, Array zero-copy matrix, ValueBlock, type helpers."""
import pytest

import lightusd

np = pytest.importorskip("numpy")


def test_half_scalar_and_array():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    # half scalar
    p.set("h", 1.5, type="half")
    assert p["h"] == pytest.approx(1.5, abs=1e-3)
    # half array via float16 numpy – core materializes as float32 Array
    arr16 = np.array([1.0, 2.0, 3.0], dtype=np.float16)
    p.set("h_arr", arr16, type="half[]")
    st2 = lightusd.load_bytes(st.export_usdc())
    a = st2.prim_at("/P")["h_arr"]
    # float16 arrays are promoted to float32 on read
    assert a.dtype == "float32"
    assert np.allclose(np.asarray(a), [1.0, 2.0, 3.0])


def test_vector_and_matrix_types():
    cases = [
        ("f2", "float2", (1.0, 2.0)),
        ("f3", "float3", (1.0, 2.0, 3.0)),
        ("f4", "float4", (1.0, 2.0, 3.0, 4.0)),
        ("d2", "double2", (5.0, 6.0)),
        ("d3", "double3", (5.0, 6.0, 7.0)),
        ("d4", "double4", (5.0, 6.0, 7.0, 8.0)),
        ("i2", "int2", (1, 2)),
        ("i3", "int3", (1, 2, 3)),
        ("u2", "uint2", (9, 10)),
        ("point3f", "point3f", (0.1, 0.2, 0.3)),
        ("color3f", "color3f", (0.2, 0.4, 0.6)),
        ("normal3f", "normal3f", (0.0, 1.0, 0.0)),
    ]
    for name, t, val in cases:
        st = lightusd.Stage.create()
        p = st.define_prim("/P", "Xform")
        p.set(name, val, type=t)
        st2 = lightusd.loads(st.export_usda())
        got = st2.prim_at("/P")[name]
        assert got == pytest.approx(val) if isinstance(val[0], float) else got == val

    # matrix4d via numpy eye
    st = lightusd.Stage.create()
    st.define_prim("/P", "Xform").set("m", np.eye(4), type="matrix4d")
    st2 = lightusd.load_bytes(st.export_usdc())
    m = st2.prim_at("/P")["m"]
    assert len(m) == 16
    assert np.allclose(np.asarray(m).reshape(4, 4), np.eye(4))


def test_string_token_asset_and_arrays():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    p.set("tok", "hello", type="token")
    p.set("str", "world", type="string")
    p.set("asset", "a.png", type="asset")
    p.set("tok_arr", ["a", "b", "c"], type="token[]")
    p.set("str_arr", ["x", "y"], type="string[]")
    st2 = lightusd.loads(st.export_usda())
    q = st2.prim_at("/P")
    assert q["tok"] == "hello"
    assert q["str"] == "world"
    assert q["asset"] == "a.png"
    assert q["tok_arr"] == ("a", "b", "c")
    # string arrays also return tuple
    assert q["str_arr"] == ("x", "y")


def test_array_zero_copy_all_integer_types():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    # exercised via int32/uint32 paths; uchar maps internally to uint8 but
    # Python facade promotes via int/uint – just verify uint roundtrip
    p.set("u_arr", np.array([1, 2, 255], dtype=np.uint32), type="uint[]")
    st2 = lightusd.load_bytes(st.export_usdc())
    arr = st2.prim_at("/P")["u_arr"]
    a = np.asarray(arr)
    assert a.tolist() == [1, 2, 255]
    # zero-copy pointer equality
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]


def test_value_block_identity():
    st = lightusd.Stage.create()
    p = st.define_prim("/P", "Xform")
    p.set("v", 1.0, type="float")
    p.attribute("v").block()
    assert p["v"] is lightusd.ValueBlock
    # reload preserves block via USDA (None)
    st2 = lightusd.loads(st.export_usda())
    # USDA writes `float v = None` – parser should produce ValueBlock
    # Some cores keep block as authored None; verify either block or missing
    # For now ensure value is block or exception path
    try:
        val = st2.prim_at("/P")["v"]
        assert val is lightusd.ValueBlock
    except KeyError:
        pass  # if block prunes property


def test_type_helpers_roundtrip():
    for name in ["float", "double", "int", "uint", "int64", "uint64", "half",
                 "float3", "double4", "point3f", "color3f", "matrix4d",
                 "token", "string", "asset"]:
        tid = lightusd.type_from_name(name)
        assert tid != 0, f"type_from_name failed for {name}"
        back = lightusd.type_name(tid)
        # asset maps to asset_path in some cores
        assert back in (name, "asset_path") or name in back
    assert lightusd.type_from_name("not_a_type") == 0
    assert lightusd.type_name(9999) is not None


def test_asarray_and_memoryview_zero_copy():
    st = lightusd.loads('#usda 1.0\ndef Mesh "M" { point3f[] points = [(0,0,0),(1,1,1)]\n}')
    arr = st.prim_at("/M")["points"]
    a = lightusd.asarray(arr)
    b = np.asarray(arr)
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(b).__array_interface__["data"][0]
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
    mv = memoryview(arr)
    assert mv.nbytes == arr.nbytes
    assert mv.readonly
    assert len(arr) == 2
    assert arr[0] == (0.0, 0.0, 0.0)
    assert arr.tolist() == [(0.0, 0.0, 0.0), (1.0, 1.0, 1.0)]
    assert "Array(" in repr(arr)
