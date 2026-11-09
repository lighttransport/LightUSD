# SPDX-License-Identifier: Apache-2.0
"""Concurrency smoke tests. Meaningful on free-threaded CPython (cp31Xt),
but also valid (GIL-serialized) on regular builds."""
import concurrent.futures as cf

import pytest

import lightusd

from conftest import SIMPLE_USDA

np = pytest.importorskip("numpy")

N_THREADS = 16
N_ITER = 8


def test_concurrent_independent_loads():
    def work(_):
        st = lightusd.loads(SIMPLE_USDA)
        total = 0.0
        for prim in st:
            if "points" in prim:
                total += float(np.asarray(prim["points"]).sum())
        st.close()
        return total

    with cf.ThreadPoolExecutor(N_THREADS) as ex:
        results = list(ex.map(work, range(N_THREADS * N_ITER)))
    assert len(set(results)) == 1


def test_concurrent_reads_shared_stage():
    st = lightusd.loads(SIMPLE_USDA)

    def work(_):
        q = st.prim_at("/World/Quad")
        pts = np.asarray(q["points"])
        attr = q.attribute("xformOp:translate")
        v = attr.get(time=12.0)
        return (float(pts.sum()), v, [p.path for p in st][0])

    with cf.ThreadPoolExecutor(N_THREADS) as ex:
        results = list(ex.map(work, range(N_THREADS * N_ITER)))
    assert len(set(results)) == 1


def test_concurrent_export():
    st = lightusd.loads(SIMPLE_USDA)

    def work(_):
        return st.export_usda()

    with cf.ThreadPoolExecutor(8) as ex:
        outs = list(ex.map(work, range(32)))
    assert len(set(outs)) == 1


def test_concurrent_clip_resolve_reuses_and_revalidates(tmp_path):
    """One stage, many interleaved queries: clip stages stay correct and fresh.

    The C API keeps a stage-lifetime value-clip stage cache. Interleaved queries
    must resolve the right value, and a rewritten clip file must be observed by
    the next query instead of serving a stale cached stage. True parallel entry
    (beyond the GIL) is covered by TestConcurrentValueClipCache in
    tests/next/test_aousd_conformance.cc.
    """
    clip = tmp_path / "clip.usda"
    clip.write_text(
        '#usda 1.0\ndef Xform "Root" { float value.timeSamples = {0: 2, 2: 6}\n}\n')
    root = tmp_path / "root.usda"
    root.write_text('''#usda 1.0
 def Xform "Root" (clips = { dictionary default = {
  asset[] assetPaths = [@clip.usda@]
  double2[] active = [(0,0)]
  string primPath = "/Root"
 } }) {\n float value\n}
''')

    with lightusd.load(root) as stage:
        attr = stage.prim_at("/Root").attribute("value")

        def resolve(t):
            return float(attr.resolve(time=t))

        def wave(expected):
            with cf.ThreadPoolExecutor(N_THREADS) as ex:
                results = list(ex.map(resolve, [0.0, 2.0] * (N_THREADS * N_ITER // 2)))
            assert results[0::2] == [expected[0]] * (len(results) // 2)
            assert results[1::2] == [expected[1]] * (len(results) // 2)

        wave((2.0, 6.0))
        # Both samples change and the file grows, so a stale cached stage is
        # detectable by content, size and mtime alike.
        clip.write_text(
            '#usda 1.0\ndef Xform "Root" { float value.timeSamples = {0: 25, 2: 60}\n}\n')
        wave((25.0, 60.0))
