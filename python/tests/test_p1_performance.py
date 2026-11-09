# SPDX-License-Identifier: Apache-2.0
"""P1: Performance / memory-budget / flatten_file micro-benchmarks.

No disk corpus required — uses synthetic meshes. Prints timing + RSS for
manual inspection (`pytest -s`). All tests assert zero-copy and bounded
memory growth.
"""
import time
try:
    import resource
except ImportError:
    resource = None
import pathlib
import pytest

import lightusd
from lightusd import tydra

np = pytest.importorskip("numpy")


def _rss_mb():
    try:
        import psutil
        return psutil.Process().memory_info().rss / (1024 * 1024)
    except Exception:
        if resource is None:
            return 0.0
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        # Linux: KB, macOS: bytes
        import sys
        if sys.platform == "darwin":
            return rss / (1024 * 1024)
        return rss / 1024.0


def test_zero_copy_view_under_5ms():
    n = 200_000
    st = lightusd.Stage.create()
    m = st.define_prim("/M", "Mesh")
    pts = np.arange(n * 3, dtype=np.float32).reshape(n, 3)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.full(n // 3, 3, dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.arange((n // 3) * 3, dtype=np.int32), type="int[]")
    blob = st.export_usdc()
    st2 = lightusd.load_bytes(blob)
    arr = st2.prim_at("/M")["points"]
    t0 = time.perf_counter()
    a = np.asarray(arr)
    t_view = (time.perf_counter() - t0) * 1000
    # zero-copy: same pointer, no copy time
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
    assert a.shape == (n, 3)
    print(f"[perf] view {t_view:.2f} ms for n={n} (expect <5 ms)")
    assert t_view < 5.0, f"zero-copy view too slow: {t_view:.2f} ms"


def test_rss_delta_bounded():
    rss0 = _rss_mb()
    n = 300_000  # ~3.6 MB points + indices
    pts = np.zeros((n, 3), np.float32)
    st = lightusd.Stage.create()
    m = st.define_prim("/M", "Mesh")
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.full(n // 3, 3, dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.arange((n // 3) * 3, dtype=np.int32), type="int[]")
    blob = st.export_usdc()
    rss1 = _rss_mb()
    st2 = lightusd.load_bytes(blob)
    # Access array without copying
    _ = np.asarray(st2.prim_at("/M")["points"])
    rss2 = _rss_mb()
    delta = rss2 - rss0
    # Delta should be < 3x nbytes (allow overhead but detect leaks/copies)
    # On macOS ru_maxrss is bytes and max, so be very lenient; use psutil if available
    import sys
    nbytes = n * 3 * 4
    print(f"[perf] rss0 {rss0:.1f} -> {rss2:.1f} MiB delta {delta:.1f} MiB nbytes {nbytes/1024/1024:.1f} MiB")
    if sys.platform == "darwin":
        assert delta < 5000, f"macOS RSS delta too large: {delta:.1f} MiB"
    else:
        assert delta < (nbytes / 1024 / 1024) * 3 + 100  # +100 MiB headroom


def test_flatten_file_vs_save(tmp_path):
    st = lightusd.Stage.create()
    st.define_prim("/A", "Xform").set("v", 1.0, type="float")
    m = st.define_prim("/A/M", "Mesh")
    pts = np.zeros((3, 3), np.float32)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([3], dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.array([0, 1, 2], dtype=np.int32), type="int[]")
    src = tmp_path / "src.usda"
    st.save(str(src))
    dst = tmp_path / "dst.usdc"
    t0 = time.perf_counter()
    lightusd.flatten_file(str(src), str(dst))
    t_flat = time.perf_counter() - t0
    assert dst.stat().st_size > 0
    assert dst.read_bytes()[:8] == b"PXR-USDC"
    print(f"[perf] flatten_file {t_flat*1000:.1f} ms")
    assert t_flat < 1.0
    # roundtrip
    st_flat = lightusd.load(str(dst))
    assert st_flat.prim_at("/A")["v"] == pytest.approx(1.0)


def test_max_memory_enforces(tmp_path):
    st = lightusd.Stage.create()
    n = 100_000
    m = st.define_prim("/M", "Mesh")
    m.set("points", np.zeros((n, 3), np.float32), type="point3f[]")
    m.set("faceVertexCounts", np.full(n // 3, 3, dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.arange((n // 3) * 3, dtype=np.int32), type="int[]")
    fn = tmp_path / "big.usdc"
    st.save(str(fn))
    # 1 KiB must fail
    with pytest.raises(lightusd.UsdError):
        lightusd.load(str(fn), max_memory=1024)
    # 200 MiB must succeed
    st2 = lightusd.load(str(fn), max_memory=200*1024*1024)
    assert st2.stats["prim_count"] >= 1


def test_tydra_zero_copy_after_flatten(tmp_path):
    st = lightusd.Stage.create()
    m = st.define_prim("/M", "Mesh")
    pts = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], np.float32)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([3], dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.array([0, 1, 2], dtype=np.int32), type="int[]")
    src = tmp_path / "src.usda"
    dst = tmp_path / "dst.usdc"
    st.save(str(src))
    lightusd.flatten_file(str(src), str(dst))
    st_flat = lightusd.load(str(dst))
    scene = tydra.to_render_scene(st_flat)
    assert len(scene.meshes) == 1
    a = np.asarray(scene.meshes[0].points)
    # tydra mesh also zero-copy
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(scene.meshes[0].points).__array_interface__["data"][0]
    print(f"[perf] tydra points {a.shape} zero-copy ok")


def test_export_load_timing_scaling():
    for n in (10_000, 100_000):
        pts = np.zeros((n, 3), np.float32)
        st = lightusd.Stage.create()
        m = st.define_prim("/M", "Mesh")
        m.set("points", pts, type="point3f[]")
        m.set("faceVertexCounts", np.full(n // 3, 3, dtype=np.int32), type="int[]")
        m.set("faceVertexIndices", np.arange((n // 3) * 3, dtype=np.int32), type="int[]")
        t0 = time.perf_counter()
        blob = st.export_usdc()
        t_exp = time.perf_counter() - t0
        t0 = time.perf_counter()
        st2 = lightusd.load_bytes(blob)
        t_load = time.perf_counter() - t0
        print(f"[perf] n={n} export {t_exp*1000:.1f}ms load {t_load*1000:.1f}ms blob {len(blob)/1024:.1f}KB")
        assert t_exp < 1.0 and t_load < 1.0
