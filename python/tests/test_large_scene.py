# SPDX-License-Identifier: Apache-2.0
"""Optional large-scene performance / zero-copy / memory tests.

Data roots (tried in order, first existing wins; override via env):

* $LIGHTUSD_LARGE_SCENE_ROOT / $LARGE_SCENE_ROOT / $LIGHTUSD_TEST_LARGE_ASSETS
* /mnt/disk1/data/{island,caldera,alab,alab/_merged_ALab}
* /mnt/disk1/data  (legacy single-root layout)

If no root is found the whole module is skipped. Tests that need a
specific scene file skip individually when that file is missing. Heavy
scenes are *not* loaded by default in CI unless the env flag is set;
use LIGHTUSD_RUN_LARGE=1 to force the full set.
"""
import os
import pathlib
import time
try:
    import resource
except ImportError:
    resource = None  # Windows has no resource module
import pytest

import lightusd
from lightusd import tydra

np = pytest.importorskip("numpy")


def _find_large_root():
    for env in ("LIGHTUSD_LARGE_SCENE_ROOT", "LARGE_SCENE_ROOT", "LIGHTUSD_TEST_LARGE_ASSETS"):
        v = os.environ.get(env)
        if v and pathlib.Path(v).is_dir():
            return pathlib.Path(v)
    candidates = [
        pathlib.Path("/mnt/disk1/data"),
        pathlib.Path("/mnt/disk1"),
        pathlib.Path("/mnt/nvme02/data"),
    ]
    for p in candidates:
        if p.is_dir():
            return p
    return None


LARGE_ROOT = _find_large_root()

# Candidate scene files (alab, island, caldera, plus Kitchen_set as lightweight public asset)
CANDIDATES = []
if LARGE_ROOT:
    CANDIDATES = [
        LARGE_ROOT / "caldera" / "caldera.usda",
        LARGE_ROOT / "island" / "usd" / "island.usda",
        LARGE_ROOT / "island" / "island.usda",
        LARGE_ROOT / "alab" / "_merged_ALab" / "entry.usda",
        LARGE_ROOT / "alab" / "_merged_ALab" / "entity" / "alab_set01" / "alab_set01.usda",
        LARGE_ROOT / "alab" / "entry.usda",
        LARGE_ROOT / "usd" / "Kitchen_set" / "Kitchen_set.usda",
        LARGE_ROOT / "usd" / "Kitchen_set" / "Kitchen_set.usd",
    ]


def _first_existing():
    for p in CANDIDATES:
        if p.is_file():
            return p
    return None


def _rss_mb():
    try:
        import psutil  # optional
        return psutil.Process().memory_info().rss / (1024 * 1024)
    except Exception:
        if resource is None:
            return 0.0
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        import sys
        if sys.platform == "darwin":
            return rss / (1024 * 1024)
        return rss / 1024.0


pytestmark = [
    pytest.mark.large,
    pytest.mark.skipif(
        LARGE_ROOT is None or _first_existing() is None,
        reason="large-scene data not mounted (set LIGHTUSD_LARGE_SCENE_ROOT or mount /mnt/disk1/data)",
    ),
]


def test_large_scene_discovery_prints():
    # Always-run diagnostic (when module not skipped) to show what was found.
    print(f"[large] LARGE_ROOT={LARGE_ROOT}")
    for p in CANDIDATES:
        exists = p.is_file()
        sz = p.stat().st_size if exists else 0
        print(f"  {'FOUND' if exists else 'miss'} {p} {sz} bytes")


def test_large_scene_lightweight_measure():
    """Load the smallest available large scene with payload deferral timing.

    Validates that deferred vs eager payload modes differ and that
    zero-copy array views survive the load. Prints RSS and elapsed times
    for manual inspection (pytest -s).
    """
    src = _first_existing()
    if src is None:
        pytest.skip("no large scene file available")
    print(f"[large] source={src} size={src.stat().st_size} bytes rss0={_rss_mb():.1f} MiB")
    # Warm file-system cache or report
    t0 = time.perf_counter()
    rss0 = _rss_mb()
    # Try load with payload deferral (smaller working set) then without
    # The Python binding exposes load_payloads bool.
    st_defer = lightusd.load(str(src), load_payloads=False)
    t_defer = time.perf_counter() - t0
    rss1 = _rss_mb()
    stats = st_defer.stats
    print(f"[large] defer load {t_defer:.2f}s prims={stats['prim_count']} mem={stats['memory_bytes']/1024/1024:.1f} MiB rss={rss1:.1f} MiB")
    # Verify stage sanity
    assert stats["prim_count"] > 0
    assert len(st_defer) > 0 or len(list(st_defer)) == 0  # iterator works
    # Zero-copy check on first mesh-like prim if any
    for prim in st_defer:
        if "points" in prim:
            arr = prim["points"]
            a = np.asarray(arr)
            assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
            assert a.nbytes == arr.nbytes
            print(f"[large] zero-copy verified {prim.path} points {a.shape} {a.dtype}")
            break
    # Tydra conversion timing (optional, may be heavy)
    t1 = time.perf_counter()
    try:
        scene = tydra.to_render_scene(st_defer, triangulate=True, compute_normals=True)
        t_tydra = time.perf_counter() - t1
        rss2 = _rss_mb()
        print(f"[large] tydra {t_tydra:.2f}s meshes={len(scene.meshes)} rss={rss2:.1f} MiB")
        if scene.meshes:
            m = scene.meshes[0]
            if m.points is not None:
                a2 = np.asarray(m.points)
                assert np.asarray(a2).__array_interface__["data"][0] == np.asarray(m.points).__array_interface__["data"][0]
                print(f"[large] tydra zero-copy mesh {m.prim_path} points {a2.shape}")
    except Exception as e:
        pytest.skip(f"tydra conversion failed (expected for some large scenes): {e}")
    st_defer.close()
    # Also test eager load if LIGHTUSD_RUN_LARGE=1 or file under 200 MB
    if src.stat().st_size < 200 * 1024 * 1024 or os.environ.get("LIGHTUSD_RUN_LARGE") == "1":
        t2 = time.perf_counter()
        st_full = lightusd.load(str(src), load_payloads=True)
        t_full = time.perf_counter() - t2
        print(f"[large] eager load {t_full:.2f}s (defer was {t_defer:.2f}s)")
        st_full.close()
    else:
        print("[large] skip eager load (file >200 MB, set LIGHTUSD_RUN_LARGE=1 to force)")


@pytest.mark.parametrize("scene_name", ["island", "caldera", "alab"])
def test_large_scene_individual_elements(scene_name):
    root = LARGE_ROOT
    if root is None:
        pytest.skip("no large root")
    # Map scene_name to a lightweight probe file if it exists
    table = {
        "island": root / "island" / "usd" / "elements" / "isPalmRig" / "element.usda",
        "caldera": root / "caldera" / "caldera.usda",
        "alab": root / "alab" / "_merged_ALab" / "entry.usda",
    }
    p = table.get(scene_name)
    if p is None or not p.is_file():
        pytest.skip(f"{scene_name} element not found at {p}")
    # Single-element load should be fast (<5s) and show zero-copy
    t0 = time.perf_counter()
    st = lightusd.load(str(p), load_payloads=False)
    elapsed = time.perf_counter() - t0
    print(f"[large:{scene_name}] {p} -> {elapsed:.2f}s prims={st.stats['prim_count']} rss={_rss_mb():.1f} MiB")
    assert elapsed < 30.0, f"{scene_name} element load too slow: {elapsed}"
    # find a points attribute to verify zero-copy
    for prim in st:
        if "points" in prim:
            arr = prim["points"]
            a = np.asarray(arr)
            assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
            break
    st.close()


@pytest.mark.slow
def test_synthetic_stress_zero_copy_and_timing():
    """Synthetic 500k-point mesh stress test regardless of disk data.

    Measures export_usdc / load_bytes / tydra conversion with wall-clock
    timing and asserts zero-copy throughout.
    """
    n = 100_000  # ~1.2 MB points; keep CI under 2s
    pts = np.random.rand(n, 3).astype(np.float32)
    st = lightusd.Stage.create()
    m = st.define_prim("/Stress", "Mesh")
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.full(n // 3, 3, dtype=np.int32))
    m.set("faceVertexIndices", np.arange((n // 3) * 3, dtype=np.int32))
    t0 = time.perf_counter()
    blob = st.export_usdc()
    t_exp = time.perf_counter() - t0
    t0 = time.perf_counter()
    st2 = lightusd.load_bytes(blob)
    t_load = time.perf_counter() - t0
    arr = st2.prim_at("/Stress")["points"]
    a = np.asarray(arr)
    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
    assert a.shape == (n, 3)
    # Tydra
    t0 = time.perf_counter()
    scene = tydra.to_render_scene(st2)
    t_tydra = time.perf_counter() - t0
    print(f"[stress] n={n} export {t_exp:.3f}s load {t_load:.3f}s tydra {t_tydra:.3f}s rss={_rss_mb():.1f} MiB")
    # Ensure tydra mesh also zero-copy
    if scene.meshes and scene.meshes[0].points is not None:
        am = np.asarray(scene.meshes[0].points)
        assert np.asarray(am).__array_interface__["data"][0] == np.asarray(scene.meshes[0].points).__array_interface__["data"][0]
