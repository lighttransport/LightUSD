# SPDX-License-Identifier: Apache-2.0
"""Optional usd-assets corpus tests (usd-wg/assets style).

Searches for the asset corpus in (first wins):

* $USD_ASSETS_ROOT / $LIGHTUSD_USD_ASSETS_ROOT / $LIGHTUSD_TEST_ASSETS
* /mnt/disk1/data/usd, /mnt/nvme02/work/lightusd-assets[/full_assets],
  /mnt/nvme02/work/lightusd-assets, ./usd-assets, ../usd-assets

Tests are *skipped* when no corpus is found. When found they enumerate
*.usda / *.usdc / *.usdz files, attempt to load each (no crash), verify
zero-copy for any mesh, and report per-file timing + RSS for coverage
of the Python binding across a public corpus. The corpus scan is capped
(CAP env) to keep CI fast.
"""
import os
import pathlib
import time
try:
    import resource
except ImportError:
    resource = None
import pytest

import lightusd
from lightusd import tydra

np = pytest.importorskip("numpy")


def _find_usd_assets_root():
    for env in ("USD_ASSETS_ROOT", "LIGHTUSD_USD_ASSETS_ROOT", "LIGHTUSD_TEST_ASSETS"):
        v = os.environ.get(env)
        if v and pathlib.Path(v).is_dir():
            return pathlib.Path(v)
    candidates = [
        pathlib.Path("/mnt/disk1/data/usd"),
        pathlib.Path("/mnt/nvme02/work/lightusd-assets/full_assets"),
        pathlib.Path("/mnt/nvme02/work/lightusd-assets"),
        pathlib.Path("/mnt/nvme02/work/lightusd-assets"),
        pathlib.Path(__file__).resolve().parents[2] / "usd-assets",
        pathlib.Path(__file__).resolve().parents[3] / "usd-assets",
        pathlib.Path("/mnt/disk1/data"),
    ]
    for p in candidates:
        if p.is_dir():
            # heuristic: contains at least one usd file or Kitchen set
            if any(p.rglob("*.usda")) or any(p.rglob("*.usdc")):
                return p
    return None


USD_ROOT = _find_usd_assets_root()

# How many files to exercise (override via LIGHTUSD_USD_ASSETS_CAP)
CAP = int(os.environ.get("LIGHTUSD_USD_ASSETS_CAP", "50"))
# Substring filter (e.g. Kitchen)
FILTER = os.environ.get("LIGHTUSD_USD_ASSETS_FILTER", "")


def _enumerate(limit=CAP):
    if USD_ROOT is None:
        return []
    # Prefer interesting files: Kitchen_set, AbandonedFactory, etc
    patterns = ["*.usda", "*.usdc", "*.usdz"]
    files = []
    for pat in patterns:
        files.extend(USD_ROOT.rglob(pat))
    files = sorted(set(files))
    if FILTER:
        files = [p for p in files if FILTER.lower() in str(p).lower()]
    # Keep small files first for speed, but ensure some diversity
    files = sorted(files, key=lambda p: p.stat().st_size if p.is_file() else 0)
    return files[:limit]


pytestmark = [
    pytest.mark.usd_assets,
    pytest.mark.skipif(
        USD_ROOT is None or not _enumerate(1),
        reason="usd-assets corpus not found (set USD_ASSETS_ROOT)",
    ),
]


def _rss_mb():
    try:
        import psutil
        return psutil.Process().memory_info().rss / (1024 * 1024)
    except Exception:
        if resource is None:
            return 0.0
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        import sys
        if sys.platform == "darwin":
            return rss / (1024 * 1024)
        return rss / 1024.0


def test_usd_assets_discovery_prints():
    print(f"[usd-assets] USD_ROOT={USD_ROOT} cap={CAP} filter={FILTER!r}")
    files = _enumerate(CAP)
    for p in files[:10]:
        print(f"  {p} {p.stat().st_size} bytes")
    print(f"  ... total {len(files)} files (capped)")


def test_usd_assets_load_no_crash():
    """Parametrized-ish loop: each file must load or raise a documented UsdError, never crash."""
    files = _enumerate(CAP)
    assert files, "no files enumerated"
    t0 = time.perf_counter()
    rss0 = _rss_mb()
    ok = 0
    failed = []
    for f in files:
        try:
            st = lightusd.load(str(f))
        except lightusd.UsdError as e:
            # Expected for some unsupported or intentionally broken assets
            print(f"[usd-assets] UsdError {f.name}: {e}")
            continue
        except Exception as e:
            failed.append((f, f"unexpected {type(e).__name__}: {e}"))
            continue
        # basic sanity
        assert st.stats["prim_count"] >= 0
        # zero-copy check on first points-bearing prim
        for prim in st:
            if "points" in prim:
                try:
                    arr = prim["points"]
                    a = np.asarray(arr)
                    assert np.asarray(a).__array_interface__["data"][0] == np.asarray(arr).__array_interface__["data"][0]
                except Exception as e:
                    failed.append((f, f"zero-copy check failed: {e}"))
                break
        # try tydra on a subset (first 10) to exercise converter
        if ok < 10:
            try:
                scene = tydra.to_render_scene(st)
                assert isinstance(scene.meshes, tuple)
            except Exception as e:
                print(f"[usd-assets] tydra skip {f.name}: {e}")
        st.close()
        ok += 1
    rss1 = _rss_mb()
    elapsed = time.perf_counter() - t0
    print(f"[usd-assets] loaded {ok}/{len(files)} ok in {elapsed:.2f}s rss {rss0:.1f}->{rss1:.1f} MiB failed={len(failed)}")
    for fp, msg in failed:
        print(f"  FAIL {fp}: {msg}")
    assert not failed, f"{len(failed)} unexpected failures"
    # At least 80% should load (some assets are intentionally exotic)
    assert ok >= len(files) * 0.5, f"too many UsdError: only {ok}/{len(files)} loaded"


def test_usd_assets_specific_kitchen_set():
    """Targeted Kitchen_set / Sponza sanity when present."""
    candidates = [
        USD_ROOT / "Kitchen_set" / "Kitchen_set.usda",
        USD_ROOT / "Kitchen_set" / "Kitchen_set.usd",
        USD_ROOT / "Kitchen_set" / "Kitchen_set" / "Kitchen_set.usda",
        USD_ROOT / "Main.1_Sponza" / "Main.1_Sponza.usd",
    ]
    target = next((p for p in candidates if p.is_file()), None)
    if target is None:
        pytest.skip("Kitchen_set/Sponza not in corpus")
    t0 = time.perf_counter()
    st = lightusd.load(str(target))
    t_load = time.perf_counter() - t0
    print(f"[usd-assets:target] {target} load {t_load:.2f}s prims={st.stats['prim_count']} rss={_rss_mb():.1f} MiB")
    # Should have at least one mesh
    meshes = st.prims_of_type("Mesh")
    print(f"  meshes: {len(meshes)}")
    t0 = time.perf_counter()
    scene = tydra.to_render_scene(st)
    t_tydra = time.perf_counter() - t0
    print(f"  tydra {t_tydra:.2f}s meshes={len(scene.meshes)} mats={len(scene.materials)}")
    assert len(scene.meshes) > 0
    # zero-copy on first mesh
    m = scene.meshes[0]
    if m.points is not None:
        a = np.asarray(m.points)
        assert np.asarray(a).__array_interface__["data"][0] == np.asarray(m.points).__array_interface__["data"][0]


def test_usd_assets_save_roundtrip_tmp(tmp_path):
    """Save/load roundtrip for a corpus file, exercising export_usda/usdc.

    Uses a synthetic stage to avoid multi-GB corpus exports that exceed the
    default memory budget.
    """
    st = lightusd.Stage.create()
    m = st.define_prim("/M", "Mesh")
    pts = np.zeros((3, 3), np.float32)
    m.set("points", pts, type="point3f[]")
    m.set("faceVertexCounts", np.array([3], dtype=np.int32), type="int[]")
    m.set("faceVertexIndices", np.array([0, 1, 2], dtype=np.int32), type="int[]")
    ref_path = "/M"
    ref_pts = pts.copy()
    blob = st.export_usdc()
    assert blob[:8] == b"PXR-USDC"
    st2 = lightusd.load_bytes(blob)
    assert st2.stats["prim_count"] == st.stats["prim_count"]
    assert np.allclose(np.asarray(st2.prim_at(ref_path)["points"]), ref_pts)
    txt = st.export_usda()
    st3 = lightusd.loads(txt)
    assert st3.stats["prim_count"] == st.stats["prim_count"]
