# SPDX-License-Identifier: Apache-2.0
import pytest

import lightusd


def test_loads_and_repr(simple_stage):
    st = simple_stage
    assert len(st) > 0
    assert "Stage(" in repr(st)
    assert st.up_axis == "Y"
    assert st.meters_per_unit == 1.0
    assert st.default_prim_path == "World"
    assert st.default_prim.name == "World"


def test_load_files(assets_dir):
    count = 0
    for name in ("cube.usda", "cube-000.usda"):
        path = assets_dir / name
        if not path.is_file():
            continue
        st = lightusd.load(path)  # accepts pathlib.Path
        assert len(st) > 0
        st.close()
        count += 1
    if not count:
        pytest.skip("no known assets present")


def test_export_reload_bytes(simple_stage):
    usdc = simple_stage.export_usdc()
    assert usdc[:8] == b"PXR-USDC"
    st = lightusd.load_bytes(usdc)
    assert st.prim_at("/World/Quad")["radius"] == 2.5

    usda = simple_stage.export_usda()
    st2 = lightusd.loads(usda)
    assert st2.prim_at("/World/Quad")["radius"] == 2.5


def test_save_load_roundtrip(tmp_path, simple_stage):
    for ext in ("usda", "usdc", "usdz"):
        fn = tmp_path / f"scene.{ext}"
        simple_stage.save(str(fn))
        assert fn.is_file()
        st = lightusd.load(fn)
        assert st.prim_at("/World/Quad")["radius"] == 2.5
        st.close()


def test_load_crate_element_limit(tmp_path, simple_stage):
    # Large trusted crates can raise the structural-table caps explicitly;
    # zero keeps the secure defaults.
    fn = tmp_path / "scene.usdc"
    simple_stage.save(str(fn))
    with lightusd.load(fn, max_crate_elements=100_000_000) as st:
        assert st.prim_at("/World/Quad")["radius"] == 2.5
    with pytest.raises(lightusd.UsdParseError, match="Too many"):
        lightusd.load(fn, max_crate_elements=1)

    usdc = simple_stage.export_usdc()
    with lightusd.load_bytes(usdc, max_crate_elements=100_000_000) as st:
        assert st.prim_at("/World/Quad")["radius"] == 2.5
    with pytest.raises(lightusd.UsdParseError, match="Too many"):
        lightusd.load_bytes(usdc, max_crate_elements=1)


def test_load_errors():
    with pytest.raises(lightusd.UsdError):
        lightusd.load("/nonexistent/path/to/file.usda")
    with pytest.raises((lightusd.UsdParseError, ValueError)):
        lightusd.load_bytes(b"\xff\xfe\x00garbage-bytes-here")
    with pytest.raises(lightusd.UsdParseError):
        lightusd.loads("#usda 1.0\ndef Xform {")  # malformed


def test_exception_hierarchy():
    assert issubclass(lightusd.UsdParseError, lightusd.UsdError)
    assert issubclass(lightusd.UsdParseError, ValueError)
    assert issubclass(lightusd.UsdIoError, lightusd.UsdError)
    assert issubclass(lightusd.UsdIoError, OSError)
    assert issubclass(lightusd.StaleHandleError, lightusd.UsdError)


def test_context_manager():
    with lightusd.loads("#usda 1.0\ndef Xform \"a\" {}") as st:
        assert "/a" in st
    with pytest.raises(lightusd.UsdError):
        st.prim_at("/a")  # closed


def test_is_usd(tmp_path, simple_stage):
    fn = tmp_path / "x.usdc"
    simple_stage.save(str(fn))
    assert lightusd.is_usd(fn)
    bad = tmp_path / "bad.usda"
    bad.write_text("not usd at all {")
    assert not lightusd.is_usd(bad)
