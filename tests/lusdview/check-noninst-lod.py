#!/usr/bin/env python3
"""lusdview raster LOD must apply to NON-instanced meshes too.

Two bugs, one test:

  1. The core loader gave every static-batched (non-instanced) mesh the running
     SCENE-bounds accumulator as its AABB, not its own bounds. Every unique mesh
     therefore claimed to span the whole scene, which silently disabled the
     per-mesh frustum cull (nothing is ever outside the frustum) and any
     size-based decision (nothing is ever small on screen).

  2. Raster LOD only ever looked at meshes with instanceCount() > 0, so unique
     geometry got no LOD at all -- no sub-pixel cull, no box-proxy substitution.

The fixture is a field of small, widely-spread cubes (each with its own material,
so the static batcher keeps them separate) plus one large cube. Auto-framed, the
small cubes are far under the proxy threshold and the large one is far over.

  --raster-lod must then collapse the small cubes into box proxies (visible mesh
  count and drawn triangles both drop sharply) while leaving the large one drawn.

If either bug returns, every mesh looks scene-sized and nothing is proxied, so
the LOD-on and LOD-off stats come out identical and this fails.

Exits 77 (skip) when the binary is missing or headless rendering is unavailable.
"""
import os
import re
import subprocess
import sys

SKIP = 77
SMALL = 64          # small cubes (should all be proxied away)
MAX_VISIBLE_FRAC = 0.25   # LOD on: at most this fraction of meshes still drawn
MIN_TRI_DROP = 0.50       # LOD on: drawn triangles must fall by at least this


def write_scene(path):
    """A field of small cubes + one big cube, each mesh with its own material."""
    def cube(name, cx, cy, cz, h, mat, color):
        pts = [(cx + sx * h, cy + sy * h, cz + sz * h)
               for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        # 6 quads over the 8 corners (index bit order: x=4, y=2, z=1).
        faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1),
                 (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
        idx = [i for f in faces for i in f]
        pstr = ", ".join(f"({x}, {y}, {z})" for x, y, z in pts)
        return f'''
  def Material "{mat}" {{
    token outputs:surface.connect = </World/{mat}/PBR.outputs:surface>
    def Shader "PBR" {{
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = ({color[0]}, {color[1]}, {color[2]})
      token outputs:surface
    }}
  }}
  def Mesh "{name}" {{
    int[] faceVertexCounts = [{", ".join(["4"] * 6)}]
    int[] faceVertexIndices = [{", ".join(str(i) for i in idx)}]
    point3f[] points = [{pstr}]
    uniform token subdivisionScheme = "none"
    rel material:binding = </World/{mat}>
  }}'''

    body = [cube("Big", 0, 0, 0, 300.0, "MatBig", (0.9, 0.9, 0.9))]
    side = 8
    for i in range(SMALL):
        gx, gz = i % side, i // side
        # Material identity is value-deduplicated by the loader. Give every
        # fixture material a distinct constant so these remain independent
        # batches and actually exercise per-mesh raster LOD.
        color = (0.2 + gx * 0.07, 0.2 + gz * 0.07,
                 0.2 + i * 0.005)
        body.append(cube(f"Small_{i}", -900 + gx * 260, 0, -900 + gz * 260, 3.0,
                         f"MatSmall_{i}", color))
    with open(path, "w") as f:
        f.write('#usda 1.0\n(defaultPrim = "World" upAxis = "Y")\n'
                'def Xform "World" {' + "".join(body) + "\n}\n")


def render(binary, scene, out, lod):
    cmd = [binary, "--core", "--headless", "--frames", "3", "--screenshot", out,
           scene]
    if lod:
        cmd.insert(3, "--raster-lod")
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=600)
    return r.stdout.decode(errors="replace")


def stats(log):
    """-> (visible_meshes, total_meshes, drawn_tris) from the headless report."""
    m = re.search(r"render stats: meshes (\d+)/(\d+) visible, .*?"
                  r"drawn tris (\d+)", log)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def main():
    if len(sys.argv) < 3:
        print("usage: check-noninst-lod.py <lusdview> <work_dir>")
        return SKIP
    binary, work = sys.argv[1], sys.argv[2]
    if not os.path.exists(binary):
        print(f"SKIP: lusdview not found at {binary}")
        return SKIP
    os.makedirs(work, exist_ok=True)
    scene = os.path.join(work, "noninst-lod.usda")
    write_scene(scene)

    off_log = render(binary, scene, os.path.join(work, "noninst_lod_off.png"),
                     lod=False)
    off = stats(off_log)
    if off is None:
        print("SKIP: no headless render stats (no usable GPU?)")
        return SKIP
    on = stats(render(binary, scene, os.path.join(work, "noninst_lod_on.png"),
                      lod=True))
    if on is None:
        print("FAIL: --raster-lod render produced no stats")
        return 1

    off_vis, off_total, off_tris = off
    on_vis, on_total, on_tris = on
    if off_total < SMALL:
        print(f"FAIL: fixture collapsed into {off_total} meshes (expected at "
              f"least {SMALL}); the static batcher merged the cubes, so this "
              f"cannot measure per-mesh LOD.")
        return 1
    if off_vis != off_total or off_tris <= 0:
        print(f"FAIL: baseline (LOD off) should draw every mesh, drew "
              f"{off_vis}/{off_total} ({off_tris} tris).")
        return 1

    vis_frac = on_vis / on_total
    tri_drop = 1.0 - (on_tris / off_tris)
    if vis_frac > MAX_VISIBLE_FRAC:
        print(f"FAIL: --raster-lod still draws {on_vis}/{on_total} meshes "
              f"({vis_frac * 100:.0f}%, need under {MAX_VISIBLE_FRAC * 100:.0f}%). "
              f"The small non-instanced cubes were not collapsed to box proxies: "
              f"either LOD skips non-instanced meshes, or every mesh carries the "
              f"scene-spanning AABB and so looks large on screen.")
        return 1
    if tri_drop < MIN_TRI_DROP:
        print(f"FAIL: --raster-lod only cut drawn triangles by "
              f"{tri_drop * 100:.0f}% ({off_tris} -> {on_tris}, need "
              f"{MIN_TRI_DROP * 100:.0f}%).")
        return 1
    if on_tris == 0:
        print("FAIL: --raster-lod culled the whole scene, including the large "
              "cube that must stay drawn at full resolution.")
        return 1

    print(f"PASS: non-instanced raster LOD: meshes {off_vis} -> {on_vis} of "
          f"{on_total} drawn, triangles {off_tris} -> {on_tris} "
          f"({tri_drop * 100:.0f}% fewer)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
