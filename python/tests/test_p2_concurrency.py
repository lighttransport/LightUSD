# SPDX-License-Identifier: Apache-2.0
"""P2: Concurrency stress beyond freethreading smoke.

Valid on both GIL and free-threaded builds; free-threaded validates
Py_mod_gil=NOT_USED (no GIL re-enable)."""
import concurrent.futures as cf

import pytest

import lightusd
from lightusd import tydra
from conftest import SIMPLE_USDA

np = pytest.importorskip("numpy")


def test_concurrent_tydra():
    st = lightusd.loads(SIMPLE_USDA)

    def work(_):
        scene = tydra.to_render_scene(st)
        return len(scene.meshes)

    with cf.ThreadPoolExecutor(8) as ex:
        res = list(ex.map(work, range(16)))
    assert all(r == 1 for r in res)


def test_concurrent_variant_loads(tmp_path):
    base = tmp_path / "v.usda"
    base.write_text('''#usda 1.0
def Xform "R" (variants = { string lod = "high" }
    prepend variantSets = ["lod"]) {
  variantSet "lod" = { "high" { float a = 1 } "low" { float a = 2 } }
}
''')

    def work(sel):
        return lightusd.load(str(base), variants={"lod": sel}).prim_at("/R")["a"]

    with cf.ThreadPoolExecutor(8) as ex:
        highs = list(ex.map(work, ["high"] * 8))
        lows = list(ex.map(work, ["low"] * 8))
    assert all(v == pytest.approx(1.0) for v in highs)
    assert all(v == pytest.approx(2.0) for v in lows)


def test_concurrent_mixed_ops(tmp_path):
    st = lightusd.loads(SIMPLE_USDA)
    # mixed read / export / tydra
    def reader(_):
        return float(np.asarray(st.prim_at("/World/Quad")["points"]).sum())

    def exporter(_):
        return st.export_usda()

    def tydraer(_):
        return len(tydra.to_render_scene(st).meshes)

    with cf.ThreadPoolExecutor(12) as ex:
        futs = []
        for _ in range(8):
            futs.append(ex.submit(reader, 0))
            futs.append(ex.submit(exporter, 0))
            futs.append(ex.submit(tydraer, 0))
        results = [f.result() for f in futs]
    assert len(results) == 24


def test_concurrent_close_vs_read():
    # close must not race with reads after close – readers should get UsdError
    st = lightusd.loads(SIMPLE_USDA)
    st.close()

    def work(_):
        with pytest.raises(lightusd.UsdError):
            st.prim_at("/World")

    with cf.ThreadPoolExecutor(8) as ex:
        list(ex.map(work, range(8)))


def test_stage_mutation_not_concurrent():
    # Structural mutation on one thread while others are blocked should be safe
    # if done sequentially; here we just verify mutation detection via generation
    st = lightusd.Stage.create()
    st.define_prim("/A", "Xform")
    a = st.prim_at("/A")
    st.define_prim("/B", "Xform")  # bumps generation
    # Old handle `a` self-heals? For structural edit, current Python binding
    # allows old handles to self-heal for define, but remove should stale.
    # Verify remove stales:
    st2 = lightusd.Stage.create()
    b = st2.define_prim("/X", "Xform")
    st2.define_prim("/Y", "Xform")
    c = st2.prim_at("/X")
    st2.remove_prim("/X")
    with pytest.raises(lightusd.StaleHandleError):
        _ = c.name
