#!/usr/bin/env python3
"""Generate the value-resolution EDGE-CASE matrix with pxr-baked expectations.

Authors small USDA cases under tests/core/fixtures/vr-edge/<case>/ and bakes
the PINNED OpenUSD oracle's resolved values into
tests/core/generated/vr-edge-expected.inc (an X-macro table consumed by
tests/core/test_vr_edge_matrix.cc). The fixtures and the .inc are COMMITTED;
re-run this script only when extending the matrix:

  PYTHONPATH=/mnt/nvme02/work/lightusd-repo/OpenUSD/dist/lib/python \
    python3 scripts/generate-aousd-vr-edge-cases.py

Coverage targets (doc/ousd-vs-lightusd.md wishlist, beyond what the supplemental
corpus asserts): per-type interpolation dispatch (linear vs held-only types),
array lerp incl. size-mismatch hold, sublayer offset/scale and
timeCodesPerSecond stage-time mapping, default-time vs numeric zero, value
blocks (default and per-sample), and pre/post extrapolation clamping.
"""

import os
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests/core/fixtures/vr-edge"
OUT_INC = REPO / "tests/core/generated/vr-edge-expected.inc"

try:
    from pxr import Usd, Sdf  # noqa: F401
except ImportError:
    sys.exit("pxr python not importable — run with PYTHONPATH="
             "/mnt/nvme02/work/lightusd-repo/OpenUSD/dist/lib/python")

# --------------------------------------------------------------------------
# case definitions: (case_name, {filename: usda_text})
# Every case has root.usda; queries below reference prim /Root.
# --------------------------------------------------------------------------

CASES = {}

CASES["interp-scalar-types"] = {"root.usda": """#usda 1.0
def "Root"
{
    float f.timeSamples = { 0: 0, 10: 10 }
    double d.timeSamples = { 0: 0, 10: 10 }
    half h.timeSamples = { 0: 0, 10: 10 }
    int i.timeSamples = { 0: 0, 10: 10 }
    int64 i64.timeSamples = { 0: 0, 10: 10 }
    uchar uc.timeSamples = { 0: 0, 10: 10 }
    uint ui.timeSamples = { 0: 0, 10: 10 }
    bool b.timeSamples = { 0: false, 10: true }
    token tok.timeSamples = { 0: "lo", 10: "hi" }
    string s.timeSamples = { 0: "lo", 10: "hi" }
    timecode tc.timeSamples = { 0: 0, 10: 10 }
    float2 f2.timeSamples = { 0: (0, 0), 10: (10, 20) }
    float3 f3.timeSamples = { 0: (0, 0, 0), 10: (10, 20, 30) }
    double3 d3.timeSamples = { 0: (0, 0, 0), 10: (10, 20, 30) }
    float4 f4.timeSamples = { 0: (0, 0, 0, 0), 10: (10, 20, 30, 40) }
    quatf q.timeSamples = { 0: (1, 0, 0, 0), 10: (0, 1, 0, 0) }
    matrix4d m.timeSamples = { 0: ( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1) ), 10: ( (2,0,0,0),(0,2,0,0),(0,0,2,0),(10,20,30,1) ) }
    color3f c3.timeSamples = { 0: (0, 0, 0), 10: (1, 2, 3) }
    normal3f n3.timeSamples = { 0: (0, 0, 1), 10: (1, 0, 0) }
    texCoord2f uv.timeSamples = { 0: (0, 0), 10: (1, 1) }
}
"""}

CASES["interp-arrays"] = {"root.usda": """#usda 1.0
def "Root"
{
    float[] same.timeSamples = { 0: [0, 100], 10: [10, 200] }
    float[] mismatch.timeSamples = { 0: [0, 100], 10: [10, 200, 300] }
    int[] ints.timeSamples = { 0: [0, 100], 10: [10, 200] }
    float3[] pts.timeSamples = { 0: [(0,0,0), (1,1,1)], 10: [(10,0,0), (1,1,11)] }
}
"""}

CASES["offset-scale"] = {
    "root.usda": """#usda 1.0
(
    subLayers = [
        @./sub.usda@ (offset = 10; scale = 2)
    ]
)
""",
    "sub.usda": """#usda 1.0
def "Root"
{
    double v.timeSamples = { 0: 0, 10: 100 }
}
""",
}

CASES["ref-offset"] = {
    "root.usda": """#usda 1.0
def "Root" (
    references = @./refd.usda@</R> (offset = 5; scale = 0.5)
)
{
}
""",
    "refd.usda": """#usda 1.0
def "R"
{
    double v.timeSamples = { 0: 0, 20: 200 }
}
""",
}

CASES["tcps-mapping"] = {
    "root.usda": """#usda 1.0
(
    timeCodesPerSecond = 24
    subLayers = [ @./sub48.usda@ ]
)
""",
    "sub48.usda": """#usda 1.0
(
    timeCodesPerSecond = 48
)
def "Root"
{
    double v.timeSamples = { 0: 0, 48: 480 }
}
""",
}

CASES["tcps-ref"] = {
    "root.usda": """#usda 1.0
(
    timeCodesPerSecond = 24
)
def "Root" (
    references = @./refd48.usda@</R>
)
{
}
""",
    "refd48.usda": """#usda 1.0
(
    timeCodesPerSecond = 48
)
def "R"
{
    double v.timeSamples = { 0: 0, 48: 480 }
}
""",
}

# A root authoring timeCodesPerSecond = 0 makes the tcps auto-scale (parent /
# sublayer) DEGENERATE: every sublayer sample time collapses onto one instant.
# `neg` starts below 0 so the baked oracle distinguishes "first sample wins"
# from "value at layer time 0" (which would interpolate toward 0 instead).
CASES["tcps-zero"] = {
    "root.usda": """#usda 1.0
(
    timeCodesPerSecond = 0
    subLayers = [ @./sub24.usda@ ]
)
""",
    "sub24.usda": """#usda 1.0
(
    timeCodesPerSecond = 24
)
def "Root"
{
    double v.timeSamples = { 0: 0, 100: 100 }
    double neg.timeSamples = { -10: -10, 100: 100 }
}
""",
}

CASES["default-vs-zero"] = {"root.usda": """#usda 1.0
def "Root"
{
    double v = 1
    double v.timeSamples = { 0: 2, 10: 3 }
    double only_default = 7
}
"""}

CASES["blocks"] = {
    "root.usda": """#usda 1.0
(
    subLayers = [ @./weak.usda@ ]
)
over "Root"
{
    double blocked = None
    double sample_blocked.timeSamples = { 0: 1, 10: None, 20: 3 }
}
""",
    "weak.usda": """#usda 1.0
def "Root"
{
    double blocked = 42
    double sample_blocked = 5
}
""",
}

CASES["extrapolation"] = {"root.usda": """#usda 1.0
def "Root"
{
    double v.timeSamples = { 10: 10, 20: 40 }
    double single.timeSamples = { 15: 5 }
}
"""}

# --------------------------------------------------------------------------
# queries: (case, prim, attr, time, interp) — time None = Default
# interp: "linear" | "held"
# --------------------------------------------------------------------------

DEFAULT = None
Q = []

for attr in ["f", "d", "h", "i", "i64", "uc", "ui", "b", "tok", "s", "tc",
             "f2", "f3", "d3", "f4", "q", "m", "c3", "n3", "uv"]:
    Q.append(("interp-scalar-types", "/Root", attr, 5.0, "linear"))
    Q.append(("interp-scalar-types", "/Root", attr, 5.0, "held"))

for attr in ["same", "mismatch", "ints", "pts"]:
    Q.append(("interp-arrays", "/Root", attr, 5.0, "linear"))
    Q.append(("interp-arrays", "/Root", attr, 5.0, "held"))

# sub.usda samples {0:0,10:100} under (offset 10, scale 2): stage times 10..30.
for t in [10.0, 20.0, 30.0, 0.0, 40.0]:
    Q.append(("offset-scale", "/Root", "v", t, "linear"))

# refd samples {0:0,20:200} under (offset 5, scale 0.5): stage times 5..15.
for t in [5.0, 10.0, 15.0]:
    Q.append(("ref-offset", "/Root", "v", t, "linear"))

# 48tcps sublayer under a 24tcps root: samples {0:0,48:480} -> stage 0..24.
for t in [0.0, 12.0, 24.0]:
    Q.append(("tcps-mapping", "/Root", "v", t, "linear"))

# 48tcps layer REFERENCED from a 24tcps root: same auto-scale across the
# reference arc.
for t in [0.0, 12.0, 24.0]:
    Q.append(("tcps-ref", "/Root", "v", t, "linear"))

# tcps = 0 root: the auto-scale is degenerate (0/24 == 0), so the sublayer is
# frozen at ONE instant. Probe across the whole range -- every stage time must
# give the same value, and `neg` pins WHICH sample survives.
for t in [0.0, 50.0, 100.0]:
    Q.append(("tcps-zero", "/Root", "v", t, "linear"))
    Q.append(("tcps-zero", "/Root", "neg", t, "linear"))

Q += [
    ("default-vs-zero", "/Root", "v", DEFAULT, "linear"),
    ("default-vs-zero", "/Root", "v", 0.0, "linear"),
    ("default-vs-zero", "/Root", "v", 5.0, "linear"),
    ("default-vs-zero", "/Root", "only_default", 3.0, "linear"),
    ("blocks", "/Root", "blocked", DEFAULT, "linear"),
    ("blocks", "/Root", "blocked", 5.0, "linear"),
    ("blocks", "/Root", "sample_blocked", 0.0, "linear"),
    ("blocks", "/Root", "sample_blocked", 5.0, "linear"),
    ("blocks", "/Root", "sample_blocked", 10.0, "linear"),
    ("blocks", "/Root", "sample_blocked", 15.0, "linear"),
    ("blocks", "/Root", "sample_blocked", 20.0, "linear"),
    ("extrapolation", "/Root", "v", 5.0, "linear"),
    ("extrapolation", "/Root", "v", 25.0, "linear"),
    ("extrapolation", "/Root", "v", 5.0, "held"),
    ("extrapolation", "/Root", "v", 25.0, "held"),
    ("extrapolation", "/Root", "single", 0.0, "linear"),
    ("extrapolation", "/Root", "single", 30.0, "linear"),
]

# --------------------------------------------------------------------------
# bake
# --------------------------------------------------------------------------

def flatten_value(v):
    """Return (kind, [doubles], str) for a resolved pxr value."""
    from pxr import Gf, Vt
    if v is None:
        return ("none", [], "")
    if isinstance(v, bool):
        return ("num", [1.0 if v else 0.0], "")
    if isinstance(v, (int, float)):
        return ("num", [float(v)], "")
    if isinstance(v, str):
        return ("str", [], v)
    if isinstance(v, Sdf.TimeCode):
        return ("num", [float(v.GetValue())], "")
    if isinstance(v, (Gf.Vec2f, Gf.Vec2d, Gf.Vec2h)):
        return ("num", [float(v[0]), float(v[1])], "")
    if isinstance(v, (Gf.Vec3f, Gf.Vec3d, Gf.Vec3h)):
        return ("num", [float(v[i]) for i in range(3)], "")
    if isinstance(v, (Gf.Vec4f, Gf.Vec4d, Gf.Vec4h)):
        return ("num", [float(v[i]) for i in range(4)], "")
    if isinstance(v, (Gf.Quatf, Gf.Quatd, Gf.Quath)):
        im = v.GetImaginary()
        return ("num", [float(v.GetReal())] +
                        [float(im[i]) for i in range(3)], "")
    if isinstance(v, (Gf.Matrix4d,)):
        return ("num", [float(v[r][c]) for r in range(4) for c in range(4)], "")
    # arrays
    try:
        n = len(v)
    except TypeError:
        raise RuntimeError(f"unhandled value type: {type(v)} = {v!r}")
    nums = []
    for e in v:
        k, d, _ = flatten_value(e)
        assert k == "num", f"array of non-numeric {type(e)}"
        nums.extend(d)
    return ("arr", nums, str(len(v)))


def main():
    FIXTURES.mkdir(parents=True, exist_ok=True)
    for case, files in CASES.items():
        d = FIXTURES / case
        d.mkdir(parents=True, exist_ok=True)
        for fname, text in files.items():
            (d / fname).write_text(text, encoding="utf-8")

    rows = []
    stages = {}
    for case, prim_path, attr, t, interp in Q:
        if (case, interp) not in stages:
            st = Usd.Stage.Open(str(FIXTURES / case / "root.usda"))
            assert st, f"pxr cannot open case {case}"
            st.SetInterpolationType(
                Usd.InterpolationTypeLinear if interp == "linear"
                else Usd.InterpolationTypeHeld)
            stages[(case, interp)] = st
        st = stages[(case, interp)]
        prim = st.GetPrimAtPath(prim_path)
        assert prim, f"{case}: no prim {prim_path}"
        a = prim.GetAttribute(attr)
        assert a, f"{case}: no attr {prim_path}.{attr}"
        tq = Usd.TimeCode.Default() if t is None else Usd.TimeCode(t)
        v = a.Get(tq)
        kind, nums, s = flatten_value(v)
        rows.append((case, prim_path, attr,
                     "default" if t is None else repr(float(t)),
                     interp, kind, nums, s))

    with open(OUT_INC, "w", encoding="utf-8") as f:
        f.write(
            "// GENERATED by scripts/generate-aousd-vr-edge-cases.py — do not"
            " edit.\n"
            "// Expected values baked from the PINNED OpenUSD 26.05 oracle\n"
            "// (PYTHONPATH=OpenUSD/dist/lib/python). Row format:\n"
            "// VR_EDGE_CASE(case, prim, attr, is_default_time, time, held,\n"
            "//              kind, num_count, str, doubles...)\n"
            "// kind: 0=no value, 1=numeric tuple, 2=string, 3=numeric array\n"
            "//       (str holds the element count for kind 3). The numeric\n"
            "//       lanes are the trailing __VA_ARGS__ (may be empty).\n")
        for case, prim, attr, t, interp, kind, nums, s in rows:
            kind_id = {"none": 0, "num": 1, "str": 2, "arr": 3}[kind]
            is_default = 1 if t == "default" else 0
            tval = "0.0" if t == "default" else t
            num_list = ", ".join(f"{x!r}" for x in nums)
            tail = f', {num_list}' if nums else ''
            f.write(f'VR_EDGE_CASE("{case}", "{prim}", "{attr}", '
                    f'{is_default}, {tval}, '
                    f'{1 if interp == "held" else 0}, {kind_id}, '
                    f'{len(nums)}, "{s}"{tail})\n')
    print(f"wrote {len(rows)} expectations to {OUT_INC}")
    print(f"fixtures under {FIXTURES}")


if __name__ == "__main__":
    main()
