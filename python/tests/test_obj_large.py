# SPDX-License-Identifier: Apache-2.0
"""Generated in-memory OBJ inputs; no large assets are stored in the repository."""
import math

import lightusd
import pytest


@pytest.mark.parametrize("buffer_type", [bytes, bytearray, memoryview])
def test_generated_many_faces(buffer_type):
    # Flat shading expands three source positions into 60,000 USD points.
    count = 20000
    source = (b"v 0 0 0\nv 1 0 0\nv 0 1 0\n"
              b"vw 0 0 .25 1 .75\nvw 1 1 1\nvw 2 0 1\n"
              + b"f 1 2 3 # polygon\n" * count)
    stage = lightusd.load_bytes(buffer_type(source), format="obj", max_memory=64 * 1024 * 1024)
    mesh = stage.prims_of_type("Mesh")[0]
    assert len(mesh["points"]) == count * 3
    assert len(mesh["faceVertexCounts"]) == count
    indices = memoryview(mesh["faceVertexIndices"])
    assert indices[-1] == count * 3 - 1
    weights = memoryview(mesh["primvars:skel:jointWeights"])
    assert len(weights) == count * 6
    assert list(weights[:6]) == [.25, .75, 1, 0, 1, 0]
    assert not stage.warnings()
    # The source fits, but the expanded geometry cannot fit in this budget.
    with pytest.raises(lightusd.UsdError, match="budget|limit"):
        lightusd.load_bytes(source, format="obj", max_memory=1024 * 1024)


@pytest.mark.parametrize("row", [b"f 1junk 2 3", b"f 2147483648 2 3",
                                 b"f -2147483648 2 3", b"f 1/0 2 3",
                                 b"vw 0 2147483648 1", b"v 0 0 nan"])
def test_numeric_overflow_and_bad_indices(row):
    with pytest.raises(lightusd.UsdError):
        lightusd.load_bytes(b"v 0 0 0\nv 1 0 0\nv 0 1 0\n" + row, format="obj")


def test_large_coordinates_and_small_decimals():
    source = b"v 1e38 0 0\nv 0 1e38 0\nv 0 0 1e38\nf 1 2 3"
    stage = lightusd.load_bytes(source, format="obj")
    mesh = stage.prims_of_type("Mesh")[0]
    for normal in memoryview(mesh["normals"]).tolist():
        assert all(math.isfinite(v) and v > .5 for v in normal)
    stage = lightusd.load_bytes(b"v .00000000000000000001 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3", format="obj")
    assert memoryview(stage.prims_of_type("Mesh")[0]["points"]).tolist()[0][0] == pytest.approx(1e-20, abs=1e-26)


@pytest.mark.parametrize("scale", [1e-40, 1e-20, 1, 1e30])
@pytest.mark.parametrize("smoothing", [False, True])
def test_generated_normals_across_scales_and_objects(scale, smoothing):
    # The first face uses authored normals; it must still contribute its area
    # to the second face's generated smoothing normals across object boundaries.
    source = (f"v 0 0 0\nv {2 * scale} 0 0\nv 0 {scale} 0\nv 0 0 {2 * scale}\n"
              "vn 0 0 -3\n" + ("s 1\n" if smoothing else "s off\n") +
              "o authored\nf 1//1 2//1 3//1\no generated\nf 1 4 2").encode()
    stage = lightusd.load_bytes(source, format="obj")
    authored, generated = stage.prims_of_type("Mesh")
    assert memoryview(authored["normals"]).tolist() == [[0, 0, -3]] * 3
    points = memoryview(generated["points"]).tolist()
    # Source values have already rounded to float, possibly subnormal.
    ratio = points[1][2] / memoryview(authored["points"]).tolist()[2][1]
    length = math.hypot(ratio, 1)
    for i, normal in enumerate(memoryview(generated["normals"]).tolist()):
        expected = [0, ratio / length, 1 / length] if smoothing and i != 1 else [0, 1, 0]
        assert normal == pytest.approx(expected, abs=1e-6)


def test_cancelling_smoothing_normals_remain_finite():
    source = b"v 0 0 0\nv 1 0 0\nv 0 1 0\ns 1\nf 1 2 3\nf 3 2 1"
    stage = lightusd.load_bytes(source, format="obj")
    assert memoryview(stage.prims_of_type("Mesh")[0]["normals"]).tolist() == [[0, 0, 0]] * 3
