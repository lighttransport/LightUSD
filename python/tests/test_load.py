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


def test_array_limit_and_failed_reference(tmp_path):
    # A compressed crate array crosses the configured count cap without a
    # large fixture. A failed dependency must fail the composed load too.
    values = ', '.join(str(i) for i in range(1024))
    with lightusd.loads('#usda 1.0\ndef Xform "Asset" {\n int[] values = [' + values + ']\n}') as stage:
        asset = tmp_path / 'asset.usdc'
        stage.save(str(asset))
    data = asset.read_bytes()
    for loader, source in ((lightusd.load, asset), (lightusd.load_bytes, data)):
        with pytest.raises(lightusd.UsdError, match='max_array_elements'):
            loader(source, max_array_elements=16)
        with loader(source, max_array_elements=2048) as stage:
            assert len(stage.prim_at('/Asset')['values']) == 1024
        with pytest.raises(OverflowError):
            loader(source, max_array_elements=-1)
    root = tmp_path / 'root.usda'
    root.write_text('#usda 1.0\ndef Xform "World" (references = @asset.usdc@</Asset>) {}\n')
    with pytest.raises(lightusd.UsdError, match='max_array_elements'):
        lightusd.load(root, max_array_elements=16)
    for format in (None, "usda"):
        with lightusd.load(root, format=format, max_array_elements=2048) as stage:
            assert len(stage.prim_at('/World')['values']) == 1024
    asset.unlink()
    with pytest.raises(lightusd.UsdError):
        lightusd.load(root)


@pytest.mark.parametrize('option', ['max_memory', 'max_crate_elements', 'max_array_elements'])
@pytest.mark.parametrize('value,error', [(-1, OverflowError), (2**64, OverflowError), (1.5, TypeError)])
def test_unsigned_load_limits_do_not_wrap(tmp_path, option, value, error):
    for loader, args in ((lightusd.load, (tmp_path/'missing.usda',)),
                         (lightusd.load_bytes, (b'#usda 1.0',)),
                         (lightusd.flatten_file, (str(tmp_path/'missing.usda'), str(tmp_path/'out.usdc')))):
        with pytest.raises(error):
            loader(*args, **{option: value})


@pytest.mark.parametrize('value,error', [(-1, OverflowError), (2**31, OverflowError), (1.5, TypeError)])
def test_thread_budget_rejects_invalid_values(tmp_path, value, error):
    for loader, args in ((lightusd.load, (tmp_path/'missing.usda',)),
                         (lightusd.load_bytes, (b'#usda 1.0',)),
                         (lightusd.flatten_file, (str(tmp_path/'missing.usda'), str(tmp_path/'out.usdc')))):
        with pytest.raises(error):
            loader(*args, max_threads=value)


@pytest.mark.parametrize('value,error', [('auto', ValueError), ('', ValueError), (None, TypeError), (1, TypeError)])
def test_input_policy_is_explicit(tmp_path, value, error):
    for loader, args in ((lightusd.load, (tmp_path/'missing.usda',)),
                         (lightusd.load_bytes, (b'#usda 1.0',)),
                         (lightusd.flatten_file, (str(tmp_path/'missing.usda'), str(tmp_path/'out.usdc')))):
        with pytest.raises(error):
            loader(*args, input_policy=value)


@pytest.mark.parametrize('policy', ['untrusted', 'trusted'])
@pytest.mark.parametrize('threads', [0, 1, 4])
def test_explicit_execution_policy_roundtrip(tmp_path, policy, threads):
    data=b'#usda 1.0\ndef Xform "World" { int[] values = [1,2,3] }'
    source=tmp_path/'source.usda';source.write_bytes(data)
    options=dict(input_policy=policy,max_threads=threads)
    for stage in (lightusd.load(source,**options),lightusd.load_bytes(data,**options)):
        with stage:
            assert list(stage.prim_at('/World')['values']) == [1,2,3]
    output=tmp_path/'out.usdc'
    lightusd.flatten_file(str(source),str(output),**options)
    with lightusd.load(output,**options) as stage:
        assert list(stage.prim_at('/World')['values']) == [1,2,3]
