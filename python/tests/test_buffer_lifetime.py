# SPDX-License-Identifier: Apache-2.0
import gc

import pytest

import lightusd
from lightusd import tydra


def test_typed_buffer_survives_mutation_and_close():
    stage = lightusd.loads('#usda 1.0\ndef Xform "P" { float3[] a = [(1,2,3),(4,5,6)] }')
    prim = stage.prim_at("/P")
    array = prim["a"]
    view = memoryview(array)
    assert view.format == "f" and view.shape == (2, 3)
    assert view.strides == (12, 4) and view.readonly
    assert not hasattr(array, "memoryview")
    assert not hasattr(array, "__array_interface__")
    prim.set("a", [(7, 8, 9)], type="float3[]")
    stage.close()
    del array, prim, stage
    gc.collect()
    assert view.tolist() == [[1, 2, 3], [4, 5, 6]]
    with pytest.raises(TypeError):
        view[0, 0] = 5


def test_numpy_buffer_survives_owner_release():
    np = pytest.importorskip("numpy")
    stage = lightusd.loads('#usda 1.0\ndef Xform "P" { int[] a = [1,2,3] }')
    source = stage.prim_at("/P")["a"]
    a = np.asarray(source)
    b = np.asarray(source)
    assert np.shares_memory(a, b) and not a.flags.writeable
    stage.close()
    del source, stage
    gc.collect()
    assert a.tolist() == [1, 2, 3]


def test_strided_and_big_endian_ingestion():
    np = pytest.importorskip("numpy")
    stage = lightusd.Stage.create()
    prim = stage.define_prim("/P", "Xform")
    source = np.arange(24, dtype=">f8").reshape(4, 6)[::-1, ::2]
    prim.set("a", source, type="float3[]")
    assert np.asarray(prim["a"]).tolist() == source.tolist()
    prim.set("ids", [2**64 - 1], type="uint64[]")
    assert prim["ids"].tolist() == [2**64 - 1]
    with pytest.raises((ValueError, OverflowError)):
        prim.set("ids", [-1], type="uint64[]")
    assert prim["ids"].tolist() == [2**64 - 1]


def test_render_buffer_retains_catalog():
    stage = lightusd.loads('#usda 1.0\ndef Mesh "M" { point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]\n int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0,1,2] }')
    scene = tydra.to_render_scene(stage)
    view = memoryview(scene.meshes[0].points)
    stage.close()
    del scene, stage
    gc.collect()
    assert view.shape == (3, 3) and view.tolist()[1] == [1, 0, 0]


@pytest.mark.parametrize('policy', ['untrusted', 'trusted'])
def test_lazy_crate_array_pin_survives_edit_close_and_unlink(tmp_path, policy):
    path=tmp_path/'source.usdc'
    with lightusd.Stage.create() as source:
        prim=source.define_prim('/M','Mesh')
        prim.set('points',[(float(i),float(i+1),float(i+2)) for i in range(2048)],type='point3f[]')
        source.save(str(path))
    stage=lightusd.load(path,composed=False,input_policy=policy)
    prim=stage.prim_at('/M')
    array=prim.attribute('points').resolve(time=0)
    view=memoryview(array)
    assert view.shape == (2048,3) and view.format == 'f' and view.readonly
    prim.set('points',[(0,0,0)],type='point3f[]')
    stage.close()
    del array,prim,stage
    path.unlink()
    gc.collect()
    assert view[0,0] == 0 and view[2047,2] == 2049
