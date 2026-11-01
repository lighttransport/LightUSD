#!/usr/bin/env python3
"""Smoke-test lusdrender output without third-party Python packages."""

import re
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path


def read_png_rgba(path):
    data = Path(path).read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise RuntimeError("not a PNG file")
    pos = 8
    width = height = None
    color_type = bit_depth = None
    idat = bytearray()
    while pos + 8 <= len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", payload[:10])
        elif typ == b"IDAT":
            idat.extend(payload)
        elif typ == b"IEND":
            break
    if width is None or height is None:
        raise RuntimeError("missing IHDR")
    if bit_depth != 8 or color_type != 6:
        raise RuntimeError(f"expected 8-bit RGBA PNG, got depth={bit_depth} color={color_type}")
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        f = raw[p]
        p += 1
        row = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            left = row[i - 4] if i >= 4 else 0
            up = prev[i]
            up_left = prev[i - 4] if i >= 4 else 0
            if f == 1:
                row[i] = (row[i] + left) & 0xFF
            elif f == 2:
                row[i] = (row[i] + up) & 0xFF
            elif f == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
            elif f == 4:
                pa = abs(up - up_left)
                pb = abs(left - up_left)
                pc = abs(left + up - 2 * up_left)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                row[i] = (row[i] + pred) & 0xFF
            elif f != 0:
                raise RuntimeError(f"unsupported PNG filter {f}")
        rows.append(bytes(row))
        prev = row
    return width, height, b"".join(rows)


def write_png_rgba(path, width, height, rgba):
    def chunk(typ, payload):
        return (
            struct.pack(">I", len(payload))
            + typ
            + payload
            + struct.pack(">I", zlib.crc32(typ + payload) & 0xFFFFFFFF)
        )

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw)))
        + chunk(b"IEND", b"")
    )


def main():
    if len(sys.argv) != 4:
        print("usage: check_lusdrender_smoke.py <lusdrender> <srcdir> <outdir>", file=sys.stderr)
        return 2
    exe = sys.argv[1]
    srcdir = Path(sys.argv[2])
    outdir = Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / "lusdrender-smoke.png"
    cmd = [
        exe,
        str(srcdir / "tests/usda/suzanne.usda"),
        str(out),
        "-w",
        "64",
        "-height",
        "64",
        "-ambient",
        "0.08",
    ]
    subprocess.run(cmd, check=True)
    w, h, rgba = read_png_rgba(out)
    if (w, h) != (64, 64):
        raise RuntimeError(f"unexpected dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("render appears blank")

    subdiv_scene = outdir / "lusdrender-subdiv.usda"
    subdiv_scene.write_text("""#usda 1.0

def Mesh "SubdivQuad"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
    color3f[] primvars:displayColor = [(0.8, 0.35, 0.15)] (
        interpolation = "constant"
    )
    uniform token subdivisionScheme = "catmullClark"
}
""")
    subdiv_out = outdir / "lusdrender-subdiv.png"
    subdiv_cmd = [
        exe,
        str(subdiv_scene),
        str(subdiv_out),
        "-w",
        "32",
        "-height",
        "32",
        "-subdiv",
        "1",
        "-stats",
    ]
    stats = subprocess.run(
        subdiv_cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if "subdivision level: 1" not in stats.stderr:
        raise RuntimeError("subdivision level was not reported in stats")
    if "triangles: 8" not in stats.stderr:
        raise RuntimeError(f"unexpected subdiv triangle stats:\n{stats.stderr}")
    w, h, rgba = read_png_rgba(subdiv_out)
    if (w, h) != (32, 32):
        raise RuntimeError(f"unexpected subdiv dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("subdivision render appears blank")

    rt_out = outdir / "lusdrender-rt-preview.png"
    rt_cmd = [
        exe,
        str(subdiv_scene),
        str(rt_out),
        "-w",
        "32",
        "-height",
        "32",
        "-rtPreview",
        "-stats",
    ]
    rt_stats = subprocess.run(
        rt_cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "rt preview: 1",
        "rt meshes: 1",
        "triangles: 2",
    ):
        if expected not in rt_stats.stderr:
            raise RuntimeError(f"missing RT preview stat {expected!r}:\n{rt_stats.stderr}")
    w, h, rgba = read_png_rgba(rt_out)
    if (w, h) != (32, 32):
        raise RuntimeError(f"unexpected RT preview dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("RT preview render appears blank")

    # PointInstancer expansion in the -rtPreview (next) path: each prototype is a
    # deduped BLAS and each visible instance a TLAS placement. Three instances of
    # a one-triangle prototype must expand to 3 visible / 1 unique triangle.
    pi_scene = outdir / "lusdrender-pointinstancer.usda"
    pi_scene.write_text("""#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{
    def PointInstancer "Instancer"
    {
        point3f[] positions = [(-3, 0, 0), (0, 0, 0), (3, 0, 0)]
        int[] protoIndices = [0, 0, 0]
        quatf[] orientations = [(0, 0, 0, 1), (0, 0, 0, 1), (0, 0, 0, 1)]
        float3[] scales = [(1, 1, 1), (1, 1, 1), (1, 1, 1)]
        rel prototypes = [</World/Instancer/Proto>]

        def Mesh "Proto"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
        }
    }
}
""")
    pi_out = outdir / "lusdrender-pointinstancer.png"
    pi_stats = subprocess.run(
        [exe, str(pi_scene), str(pi_out), "-w", "32", "-height", "32",
         "-rtPreview", "-stats"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "rt instancing: tlas",
        "rt point instancers: 1",
        "rt point instances: 3",
        "rt unique triangles: 1",
        "triangles: 3",
    ):
        if expected not in pi_stats.stderr:
            raise RuntimeError(
                f"missing PointInstancer stat {expected!r}:\n{pi_stats.stderr}")
    w, h, rgba = read_png_rgba(pi_out)
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("PointInstancer RT preview render appears blank")

    # Nested instancing: a PointInstancer whose prototype ITSELF contains a
    # PointInstancer. The TLAS expresses one level, so nested placements are
    # flattened (composed with each outer transform) while geometry stays deduped.
    # 2 outer clumps x 3 inner triangles = 6 visible from 1 unique triangle.
    nest_scene = outdir / "lusdrender-nested-instancer.usda"
    nest_scene.write_text("""#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{
    def PointInstancer "Outer"
    {
        point3f[] positions = [(-4, 0, 0), (4, 0, 0)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/Outer/Clump>]

        def Xform "Clump"
        {
            def PointInstancer "Inner"
            {
                point3f[] positions = [(-1.5, 0, 0), (0, 0, 0), (1.5, 0, 0)]
                int[] protoIndices = [0, 0, 0]
                rel prototypes = [</World/Outer/Clump/Inner/Proto>]

                def Mesh "Proto"
                {
                    int[] faceVertexCounts = [3]
                    int[] faceVertexIndices = [0, 1, 2]
                    point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0, 0.5, 0)]
                }
            }
        }
    }
}
""")
    nest_out = outdir / "lusdrender-nested-instancer.png"
    nest_stats = subprocess.run(
        [exe, str(nest_scene), str(nest_out), "-w", "32", "-height", "32",
         "-rtPreview", "-stats", "-autoframe"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "rt instancing: tlas",
        "rt nested instances: 6",
        "rt instances: 6",
        "rt unique triangles: 1",
        "triangles: 6",
    ):
        if expected not in nest_stats.stderr:
            raise RuntimeError(
                f"missing nested-instancer stat {expected!r}:\n{nest_stats.stderr}")
    w, h, rgba = read_png_rgba(nest_out)
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("nested-instancer RT preview render appears blank")

    # Native-instance prototype HOLDER: when N instanceable siblings share a
    # prototype, composition designates one as the prototype source ("holder"). It
    # is still a placed instance (Pixar renders all N), so it must render at its own
    # transform -- 2 siblings -> 2 visible, 1 unique. (Regression: the holder used to
    # be skipped, dropping one of the N.)
    holder_proto = outdir / "lusdrender-holder-proto.usda"
    holder_proto.write_text("""#usda 1.0
(defaultPrim = "Proto")
def Xform "Proto"
{
    def Mesh "M"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
    }
}
""")
    holder_scene = outdir / "lusdrender-holder.usda"
    holder_scene.write_text("""#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World"
{
    def Xform "A" (
        instanceable = true
        references = @./lusdrender-holder-proto.usda@</Proto>
    )
    {
        double3 xformOp:translate = (-2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def Xform "B" (
        instanceable = true
        references = @./lusdrender-holder-proto.usda@</Proto>
    )
    {
        double3 xformOp:translate = (2, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
""")
    holder_out = outdir / "lusdrender-holder.png"
    holder_stats = subprocess.run(
        [exe, str(holder_scene), str(holder_out), "-w", "48", "-height", "32",
         "-rtPreview", "-stats", "-autoframe"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "rt instancing: tlas",
        "rt instances: 2",
        "rt unique triangles: 1",
        "triangles: 2",
    ):
        if expected not in holder_stats.stderr:
            raise RuntimeError(
                f"missing holder stat {expected!r}:\n{holder_stats.stderr}")

    # Nested instanced CURVES: a PointInstancer whose prototype contains both a mesh
    # and a nested PointInstancer scattering a BasisCurves prototype. The nested
    # curve scatter must be flattened with the outer placements (2 clumps x 3 hair =
    # 6 curve instances), not collapsed to one curve per clump.
    ncurve_scene = outdir / "lusdrender-nested-curve.usda"
    ncurve_scene.write_text("""#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World"
{
    def PointInstancer "outer"
    {
        point3f[] positions = [(0, 0, 0), (10, 0, 0)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/outer/Clump>]
        def Xform "Clump"
        {
            def Mesh "trunk"
            {
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
                point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
            }
            def PointInstancer "hair"
            {
                point3f[] positions = [(0, 0, 0), (0.5, 0, 0), (1, 0, 0)]
                int[] protoIndices = [0, 0, 0]
                rel prototypes = [</World/outer/Clump/hair/Strand>]
                def BasisCurves "Strand"
                {
                    uniform token type = "linear"
                    int[] curveVertexCounts = [2]
                    point3f[] points = [(0, 0, 0), (0, 2, 0)]
                    float[] widths = [0.1, 0.1]
                }
            }
        }
    }
}
""")
    ncurve_out = outdir / "lusdrender-nested-curve.png"
    ncurve_stats = subprocess.run(
        [exe, str(ncurve_scene), str(ncurve_out), "-w", "48", "-height", "32",
         "-rtPreview", "-stats", "-autoframe"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "rt instancing: tlas",
        "rt curve instances: 6",
        "triangles: 2",
    ):
        if expected not in ncurve_stats.stderr:
            raise RuntimeError(
                f"missing nested-curve stat {expected!r}:\n{ncurve_stats.stderr}")

    # active = false prunes a prim (and its subtree). Two meshes, one inactive ->
    # only one renders.
    active_scene = outdir / "lusdrender-active.usda"
    active_scene.write_text("""#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World"
{
    def Mesh "visible"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
    }
    def Mesh "hidden" (active = false)
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(2, -1, 0), (4, -1, 0), (3, 1, 0)]
    }
}
""")
    active_out = outdir / "lusdrender-active.png"
    active_stats = subprocess.run(
        [exe, str(active_scene), str(active_out), "-w", "32", "-height", "32",
         "-rtPreview", "-stats", "-autoframe"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in ("rt meshes: 1", "triangles: 1"):
        if expected not in active_stats.stderr:
            raise RuntimeError(
                f"active=false not honored ({expected!r}):\n{active_stats.stderr}")

    # The public curve fixture has two mesh triangles. Force one triangle per
    # native BVH to exercise global primitive offsets in primary and shadow
    # rays, while retaining the same rendered result as the single-BVH path.
    mesh_chunk_scene = srcdir / "tests" / "usda" / "curves.usda"
    mesh_chunk_out = outdir / "lusdrender-triangle-chunks.png"
    mesh_chunk_stats = subprocess.run(
        [exe, str(mesh_chunk_scene), str(mesh_chunk_out), "-w", "32",
         "-height", "32", "-rtPreview", "-stats"],
        env={**os.environ, "LUSDR_TRIANGLE_CHUNK": "1"},
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if "native triangle BVHs: 2 chunk(s)" not in mesh_chunk_stats.stderr:
        raise RuntimeError(
            "triangle BVH chunking was not exercised:\n"
            + mesh_chunk_stats.stderr)
    w, h, rgba = read_png_rgba(mesh_chunk_out)
    if w != 32 or h <= 0:
        raise RuntimeError(f"unexpected triangle-chunk dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("triangle-chunk RT preview render appears blank")

    # BasisCurves ray tracing in the -rtPreview path (curves build into the
    # DirectScene as LightRT hair strands), including curve-prototype instancing
    # (a PointInstancer whose prototype is a BasisCurves is baked per instance).
    curve_scene = outdir / "lusdrender-curves.usda"
    curve_scene.write_text("""#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)
def Xform "World"
{
    def Mesh "Ground"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-10, 0, -10), (10, 0, -10), (0, 0, 10)]
    }
    def BasisCurves "Hair"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [3]
        point3f[] points = [(-6, 0, 0), (-6, 3, 0), (-6, 6, 0)]
        float[] widths = [0.3, 0.2, 0.1]
    }
    def BasisCurves "MalformedHair"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [4]
        point3f[] points = [(6, 0, 0), (6, 3, 0), (6, 6, 0)]
    }
    def PointInstancer "Grass"
    {
        point3f[] positions = [(0, 0, 0), (4, 0, 0)]
        int[] protoIndices = [0, 0]
        rel prototypes = [</World/Grass/Blade>]
        def BasisCurves "Blade"
        {
            uniform token type = "linear"
            int[] curveVertexCounts = [3]
            point3f[] points = [(0, 0, 0), (0, 2, 0), (0, 4, 0)]
            float[] widths = [0.2, 0.15, 0.05]
        }
    }
}
""")
    curve_out = outdir / "lusdrender-curves.png"
    curve_stats = subprocess.run(
        [exe, str(curve_scene), str(curve_out), "-w", "64", "-height", "64",
         "-rtPreview", "-stats", "-autoframe"],
        env={**os.environ, "LUSDR_CURVE_CHUNK": "1"},
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    # 1 direct BasisCurves "Hair" (DirectScene) + a curve-prototype instancer
    # whose Blade curve geometry is stored once and TLAS-instanced 2×.
    for expected in (
        # The direct Hair prim is one strand, so it remains whole even though
        # its two segments exceed the one-segment limit.
        "native curves: round 1 chunk(s)",
        "rt curve strands: 2",
        "rt skipped curves: 1 (invalid data: 1)",
        "rt curve instances: 2",
        "rt point instances: 2",
        "rt instancing: tlas",
    ):
        if expected not in curve_stats.stderr:
            raise RuntimeError(
                f"missing curve stat {expected!r}:\n{curve_stats.stderr}")
    w, h, rgba = read_png_rgba(curve_out)
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("curve RT preview render appears blank")

    # Gaussian splats must remain native ellipse carriers and must not require
    # one monolithic BVH. Force one sample per chunk so this also exercises the
    # chunk offset/nearest-hit path on both the CPU and Vulkan smoke profiles.
    gaussian_scene = srcdir / "tests" / "usda" / "lusdview-gaussian-splat.usda"
    gaussian_out = outdir / "lusdrender-gaussian-splat.png"
    gaussian_stats = subprocess.run(
        [exe, str(gaussian_scene), str(gaussian_out), "-w", "32", "-height", "32",
         "-rtPreview", "-stats", "-autoframe"],
        env={**os.environ, "LUSDR_GAUSSIAN_CHUNK": "1"},
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if "native Gaussian ellipses: 2 in 2 chunk(s)" not in gaussian_stats.stderr:
        raise RuntimeError(
            "Gaussian ellipse chunking was not exercised:\n"
            + gaussian_stats.stderr)
    w, h, rgba = read_png_rgba(gaussian_out)
    if w != 32 or h <= 0:
        raise RuntimeError(f"unexpected Gaussian dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("Gaussian RT preview render appears blank")

    # HIP uses native Gaussian ellipses for a pure splat scene and the bounded
    # triangle fallback for a mixed scene. A host without ROCm is still a valid
    # smoke environment: geometry selection must happen before backend startup,
    # then a recognized runtime-unavailable diagnostic is sufficient.
    hip_gaussian_out = outdir / "lusdrender-hip-gaussian-splat.png"
    hip_gaussian = subprocess.run(
        [exe, str(gaussian_scene), str(hip_gaussian_out), "-hip", "-w", "16",
         "-height", "16", "-stats"],
        env={**os.environ, "LUSDR_GAUSSIAN_CHUNK": "1",
             "LUSDR_GPU_TRIANGLE_CHUNK": "1"},
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    hip_log = hip_gaussian.stdout + hip_gaussian.stderr
    if "Requested GPU backend not built in" in hip_log:
        # GPU tests can be excluded from a CPU-only build. Keep the CPU smoke
        # coverage useful without treating the intentionally absent backend as
        # a renderer failure or attempting GPU initialization.
        print("HIP Gaussian smoke: SKIP (backend not built)")
        hip_log = ""
    if hip_log:
        native_gaussian = "native Gaussian ellipses: 2 in 2 chunk(s)" in hip_log
        fallback_gaussian = "[gpu] gaussian splats:" in hip_log
        if not native_gaussian and not fallback_gaussian:
            raise RuntimeError(
                "HIP Gaussian carrier path was not selected:\n" + hip_log)
        if fallback_gaussian and "GPU chunks: 2" not in hip_log:
            raise RuntimeError(
                "HIP Gaussian fallback did not honor the bounded chunk limit:\n" +
                hip_log)
        hip_unavailable = (
            "HIP ray tracing unavailable" in hip_log or
            "Failed to create LightRT HIP Gaussian engine" in hip_log
        )
        if hip_gaussian.returncode != 0 and not hip_unavailable:
            raise RuntimeError("unexpected HIP Gaussian failure:\n" + hip_log)
        if hip_gaussian_out.exists():
            w, h, rgba = read_png_rgba(hip_gaussian_out)
            if (w, h) != (16, 16):
                raise RuntimeError(f"unexpected HIP Gaussian dimensions {(w, h)}")

    # primvars:displayColor (constant) as base color + primvars:displayOpacity
    # (constant) see-through blend: a 0.5-opacity green quad in front of an opaque
    # red quad -> the overlap blends to a red+green mix (both channels present).
    disp_scene = outdir / "lusdrender-display.usda"
    disp_scene.write_text("""#usda 1.0
(
    defaultPrim = "root"
    upAxis = "Y"
)
def Xform "root"
{
    def Mesh "back_red" {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 2), (2, -2, 2), (2, 2, 2), (-2, 2, 2)]
        color3f[] primvars:displayColor = [(1, 0, 0)] (interpolation = "constant")
    }
    def Mesh "front_green_glass" {
        uniform bool doubleSided = 1
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1.5, -1.5, -2), (1.5, -1.5, -2), (1.5, 1.5, -2), (-1.5, 1.5, -2)]
        color3f[] primvars:displayColor = [(0, 1, 0)] (interpolation = "constant")
        float[] primvars:displayOpacity = [0.5] (interpolation = "constant")
    }
}
""")
    disp_out = outdir / "lusdrender-display.png"
    subprocess.run(
        [exe, str(disp_scene), str(disp_out), "-w", "64", "-height", "64",
         "-rtPreview", "-viewDir", "0,0,-1", "-fitScale", "1.4", "-ambient", "1"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    w, h, rgba = read_png_rgba(disp_out)
    cx, cy = w // 2, h // 2
    o = (cy * w + cx) * 4
    cr, cg, cb = rgba[o], rgba[o + 1], rgba[o + 2]
    # Front glass is green, back is red; the blended overlap must show BOTH the
    # green (displayColor) and red-through (displayOpacity) — not pure green.
    if not (cg > 40 and cr > 40):
        raise RuntimeError(
            f"displayColor/displayOpacity blend wrong: center RGB=({cr},{cg},{cb})")

    # Per-vertex displayColor: a quad with a distinct color per vertex must
    # interpolate (opposite corners differ markedly).
    vcol_scene = outdir / "lusdrender-vcolor.usda"
    vcol_scene.write_text("""#usda 1.0
(
    defaultPrim = "root"
    upAxis = "Y"
)
def Mesh "root"
{
    uniform bool doubleSided = 1
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-2, -2, 0), (2, -2, 0), (2, 2, 0), (-2, 2, 0)]
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0)] (interpolation = "vertex")
}
""")
    vcol_out = outdir / "lusdrender-vcolor.png"
    subprocess.run(
        [exe, str(vcol_scene), str(vcol_out), "-w", "64", "-height", "64",
         "-rtPreview", "-viewDir", "0,0,-1", "-fitScale", "1.1", "-ambient", "1"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    w, h, rgba = read_png_rgba(vcol_out)
    # Sample inside the quad near opposite corners; per-vertex interpolation must
    # produce clearly different colors (constant would make them identical).
    def px(fx, fy):
        x, y = int(w * fx), int(h * fy)
        o = (y * w + x) * 4
        return (rgba[o], rgba[o + 1], rgba[o + 2])
    p_bl, p_tr = px(0.3, 0.7), px(0.7, 0.3)
    if sum(abs(a - b) for a, b in zip(p_bl, p_tr)) < 60:
        raise RuntimeError(
            f"per-vertex displayColor not interpolated: {p_bl} vs {p_tr}")

    # -smooth: authored normals interpolated for smooth shading. A coarse sphere
    # with radial per-vertex normals must shade differently with -smooth (round)
    # than without (faceted).
    import math
    nu, nv = 12, 8
    sp_pts, sp_nrm = [], []
    for j in range(nv + 1):
        th = math.pi * j / nv
        for i in range(nu):
            ph = 2 * math.pi * i / nu
            v = (math.sin(th) * math.cos(ph), math.cos(th),
                 math.sin(th) * math.sin(ph))
            sp_pts.append(v)
            sp_nrm.append(v)
    sp_cnt, sp_idx = [], []
    for j in range(nv):
        for i in range(nu):
            sp_cnt.append(4)
            sp_idx += [j * nu + i, j * nu + (i + 1) % nu,
                       (j + 1) * nu + (i + 1) % nu, (j + 1) * nu + i]
    fmt3 = lambda seq: ", ".join("(%g, %g, %g)" % p for p in seq)
    sph_scene = outdir / "lusdrender-sphere.usda"
    sph_scene.write_text(
        '#usda 1.0\n( defaultPrim = "s" upAxis = "Y" )\n'
        'def Mesh "s" {\n'
        f'    int[] faceVertexCounts = [{", ".join(map(str, sp_cnt))}]\n'
        f'    int[] faceVertexIndices = [{", ".join(map(str, sp_idx))}]\n'
        f'    point3f[] points = [{fmt3(sp_pts)}]\n'
        f'    normal3f[] normals = [{fmt3(sp_nrm)}] (interpolation = "vertex")\n'
        '}\n')
    sph_a, sph_b = outdir / "lusdrender-faceted.png", outdir / "lusdrender-smooth.png"
    base_args = [str(sph_scene), "-w", "80", "-height", "80", "-rtPreview",
                 "-autoframe", "-ambient", "0.3"]
    subprocess.run([exe] + base_args[:1] + [str(sph_a)] + base_args[1:],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    subprocess.run([exe] + base_args[:1] + [str(sph_b)] + base_args[1:] +
                   ["-smooth"], check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE)
    _, _, fac = read_png_rgba(sph_a)
    _, _, smo = read_png_rgba(sph_b)
    if bytes(fac) == bytes(smo):
        raise RuntimeError("-smooth did not change shading vs faceted")

    env_png = outdir / "lusdrender-env.png"
    env_pixels = bytearray()
    for y in range(2):
        for x in range(4):
            env_pixels.extend([
                32 + x * 45,
                64 + y * 80,
                180 - x * 25,
                255,
            ])
    write_png_rgba(env_png, 4, 2, env_pixels)
    ibl_scene = outdir / "lusdrender-ibl.usda"
    ibl_scene.write_text("""#usda 1.0

def Xform "World"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        color3f[] primvars:displayColor = [(0.65, 0.65, 0.65)] (
            interpolation = "constant"
        )
    }
    def DomeLight "Sky"
    {
        asset inputs:texture:file = @lusdrender-env.png@
        token inputs:texture:format = "latlong"
        float inputs:intensity = 1
    }
}
""")
    ibl_out = outdir / "lusdrender-ibl.png"
    ibl_cmd = [
        exe,
        str(ibl_scene),
        str(ibl_out),
        "-w",
        "32",
        "-height",
        "32",
        "-ambient",
        "0.0",
        "-stats",
    ]
    ibl_stats = subprocess.run(
        ibl_cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "domelight: 1",
        "ibl envmap: 1",
        "ibl diffuse size: 512",
        "ibl prefilter levels: 5",
        "ibl brdf lut size: 4096",
    ):
        if expected not in ibl_stats.stderr:
            raise RuntimeError(f"missing IBL stat {expected!r}:\n{ibl_stats.stderr}")
    w, h, rgba = read_png_rgba(ibl_out)
    if (w, h) != (32, 32):
        raise RuntimeError(f"unexpected IBL dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("IBL render appears blank")

    direct_scene = outdir / "lusdrender-direct-prims.usda"
    direct_scene.write_text("""#usda 1.0

def Xform "World"
{
    def Sphere "Ball"
    {
        double radius = 0.45
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (-1.2, 0, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Cylinder "Tube"
    {
        double radius = 0.25
        double height = 1.2
        uniform token axis = "Y"
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Cone "Hat"
    {
        double radius = 0.35
        double height = 1.0
        uniform token axis = "Y"
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (1.1, 0, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Capsule "Pill"
    {
        double radius = 0.18
        double height = 0.9
        uniform token axis = "Y"
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (1.8, 0, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Cube "Box"
    {
        double size = 0.5
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (-1.8, -0.7, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Plane "Ground"
    {
        double width = 3.5
        double length = 0.35
        uniform token axis = "Z"
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, -1.05, 0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Points "Dots"
    {
        point3f[] points = [(-1.4, 0.75, 0), (-1.1, 0.85, 0), (-0.8, 0.78, 0)]
        float[] widths = [0.12, 0.10, 0.12]
    }
    def TetMesh "Tet"
    {
        point3f[] points = [(-1.55, -0.2, 0), (-1.15, -0.2, 0), (-1.35, 0.25, 0), (-1.35, 0.0, 0.45)]
        int4[] tetVertexIndices = [(0, 1, 2, 3)]
    }
    def Xform "Proto"
    {
        def Cube "ProtoBox"
        {
            double size = 0.18
        }
    }
    def PointInstancer "Instancer"
    {
        rel prototypes = </World/Proto>
        int[] protoIndices = [0, 0]
        point3f[] positions = [(0.9, 0.85, 0), (1.25, 0.85, 0)]
        float3[] scales = [(1, 1, 1), (1.5, 1.5, 1.5)]
    }
    def BasisCurves "Hair"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [3]
        point3f[] points = [(-0.6, -0.8, 0), (-0.55, 0.0, 0.05), (-0.4, 0.8, 0)]
        float[] widths = [0.08, 0.08, 0.08]
    }
    def BasisCurves "Ribbon"
    {
        uniform token type = "linear"
        int[] curveVertexCounts = [2]
        point3f[] points = [(0.35, -0.75, 0), (0.55, 0.75, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1)]
        float[] widths = [0.12, 0.12]
    }
    def HermiteCurves "Hermite"
    {
        int[] curveVertexCounts = [2]
        point3f[] points = [(0.75, -0.75, 0), (0.95, 0.65, 0)]
        vector3f[] tangents = [(0.8, 0.4, 0.25), (-0.5, 0.6, -0.15)]
        float[] widths = [0.08, 0.10]
    }
    def NurbsPatch "Patch"
    {
        int uVertexCount = 3
        int vVertexCount = 3
        int uOrder = 2
        int vOrder = 2
        double[] uKnots = [0, 0, 1, 2, 2]
        double[] vKnots = [0, 0, 1, 2, 2]
        point3f[] points = [(-0.5, -0.4, -0.45), (0, -0.5, -0.55), (0.5, -0.4, -0.45),
                            (-0.5, 0, -0.55), (0, 0.1, -0.65), (0.5, 0, -0.55),
                            (-0.5, 0.4, -0.45), (0, 0.5, -0.55), (0.5, 0.4, -0.45)]
    }
    def DomeLight "Sky"
    {
        color3f inputs:color = (0.03, 0.05, 0.08)
        float inputs:intensity = 1
    }
    def RectLight "Panel"
    {
        color3f inputs:color = (1, 0.85, 0.65)
        float inputs:intensity = 80
        float inputs:width = 1.0
        float inputs:height = 0.6
        matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 1.6, 1.0, 1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }
    def Mesh "Emitter" (
        prepend apiSchemas = ["MeshLightAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.2, 1.1, 0.2), (0.2, 1.1, 0.2), (0.2, 1.1, -0.2), (-0.2, 1.1, -0.2)]
        color3f inputs:color = (0.8, 0.95, 1.0)
        float inputs:intensity = 25
    }
}
""")
    direct_out = outdir / "lusdrender-direct-prims.png"
    direct_cmd = [
        exe,
        str(direct_scene),
        str(direct_out),
        "-w",
        "64",
        "-height",
        "64",
        "-ambient",
        "0.15",
        "-stats",
    ]
    stats = subprocess.run(
        direct_cmd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    for expected in (
        "direct spheres: 1",
        "direct round curve segments: 2",
        "direct flat curve segments: 1",
        "direct Hermite/Bezier curve segments: 1",
        "direct points: 3",
        "direct tetrahedra: 1",
        "direct analytic shapes: 3",
        "lights: 1",
        "mesh light triangles: 2",
        "domelight: 1",
        "light sampling finite cdf entries: 1",
        "light sampling mesh cdf entries: 2",
    ):
        if expected not in stats.stderr:
            raise RuntimeError(f"missing direct primitive stat {expected!r}:\n{stats.stderr}")
    w, h, rgba = read_png_rgba(direct_out)
    if (w, h) != (64, 64):
        raise RuntimeError(f"unexpected direct dimensions {(w, h)}")
    pixels = [rgba[i:i + 4] for i in range(0, len(rgba), 4)]
    if len(set(pixels)) <= 1:
        raise RuntimeError("direct primitive render appears blank")

    check_vulkan(exe, srcdir, outdir)
    return 0


def _tri_count(log):
    """Parse 'triangles: N' from lusdrender's stderr, or None."""
    for ln in log.splitlines():
        m = re.search(r"triangles:\s*(\d+)", ln)
        if m:
            return int(m.group(1))
    return None


def check_vulkan(exe, srcdir, outdir):
    """Vulkan GPU backend (-vk / -vkr) run + correctness test.

    The -vk (compute trace) and -vkr (hardware ray-query) backends traverse the
    same geometry as the CPU -rtPreview path, so a correct GPU render must:
      * report the SAME triangle count as the CPU path (regression guard for the
        polygon-triangulation fix -- quads must be split, else Suzanne renders
        with holes at 656 of its 968 triangles);
      * be non-blank (regression guard for the ray-query BLAS, which was fed the
        wrong vertex array and rendered fully blank);
      * agree with each other almost exactly (compute vs hardware-RT cross-check);
      * share the CPU path's framing/dimensions.
    It does not require a pixel match to the CPU image -- the GPU path flat-shades
    with geometric normals, so it is darker -- but the above pin down a correct
    render. See doc/lusdrender.md.

    SKIPs gracefully (prints a note, returns) when no Vulkan device/driver is
    available -- lusdrender then fails to create the engine and exits nonzero --
    so the smoke test still passes on headless machines without Vulkan.
    """
    scene = srcdir / "tests/usda/suzanne.usda"
    if not scene.exists():
        print("vulkan: SKIP (suzanne.usda not found)")
        return

    # 64x64 autoframe keeps the GPU trace quick; -autoframe makes the GPU path use
    # the same record camera as -rtPreview, so dimensions/framing line up.
    size = ["-w", "64", "-height", "64", "-autoframe", "-ambient", "0.1"]
    ref_out = outdir / "lusdrender-vk-ref.png"
    ref = subprocess.run([exe, str(scene), str(ref_out), *size, "-rtPreview"],
                         check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True)
    ref_tris = _tri_count(ref.stdout + ref.stderr)
    rw, rh, _ = read_png_rgba(ref_out)

    vk_rgba = {}
    for flag, name in (("-vk", "compute trace"), ("-vkr", "ray query")):
        out = outdir / f"lusdrender{flag}.png"
        run = subprocess.run([exe, str(scene), str(out), *size, flag],
                             env={**os.environ, "LUSDR_GPU_TRIANGLE_CHUNK": "400"},
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        log = run.stdout + run.stderr
        if run.returncode != 0 or not out.exists():
            # No Vulkan device / driver in this environment -> not a failure.
            print(f"vulkan {flag}: SKIP (no Vulkan device; rc={run.returncode})")
            continue
        if "backend: LightRT VK" not in log:
            raise RuntimeError(f"{flag}: expected 'backend: LightRT VK' in output:\n{log}")
        if "GPU chunks)" not in log:
            raise RuntimeError(f"{flag}: GPU mesh chunk path was not exercised:\n{log}")
        w, h, rgba = read_png_rgba(out)
        if (w, h) != (rw, rh):
            raise RuntimeError(
                f"{flag}: dimensions {(w, h)} != CPU reference {(rw, rh)} "
                f"(camera/framing mismatch)")
        if len({rgba[i:i + 4] for i in range(0, len(rgba), 4)}) <= 1:
            raise RuntimeError(f"{flag}: Vulkan render is blank (no hits)")
        tris = _tri_count(log)
        if ref_tris is not None and tris is not None and tris != ref_tris:
            raise RuntimeError(
                f"{flag}: triangle count {tris} != CPU reference {ref_tris} "
                f"(polygons not triangulated -> holey render)")
        device = next((ln for ln in log.splitlines() if "Vulkan device:" in ln), "").strip()
        print(f"vulkan {flag} ({name}): PASS ({w}x{h}, {tris} tris; {device})")
        vk_rgba[flag] = rgba

    # Compute trace and hardware ray-query are independent paths over the same
    # geometry; they must agree almost exactly.
    if "-vk" in vk_rgba and "-vkr" in vk_rgba:
        diff = _mean_abs_diff_rgb(vk_rgba["-vk"], vk_rgba["-vkr"])
        if diff > 3.0:
            raise RuntimeError(
                f"-vk vs -vkr mean abs diff {diff:.1f} > 3.0 "
                f"(compute trace and ray query disagree)")
        print(f"vulkan: -vk vs -vkr agree (mean abs diff {diff:.2f})")


def _mean_abs_diff_rgb(a_rgba, b_rgba):
    """Mean absolute per-channel difference (RGB only) of two equal-size RGBA buffers."""
    n = min(len(a_rgba), len(b_rgba))
    total = 0
    for i in range(0, n, 4):
        for c in range(3):
            total += abs(a_rgba[i + c] - b_rgba[i + c])
    return total / (n // 4 * 3) if n else 0.0


if __name__ == "__main__":
    raise SystemExit(main())
