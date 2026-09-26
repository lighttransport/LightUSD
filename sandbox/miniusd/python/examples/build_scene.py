"""Build a small textured scene from scratch and export it as .usda/.usdc/.usdz.

    python examples/build_scene.py [out_dir]
"""

import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import miniusd  # noqa: E402
from miniusd import geom  # noqa: E402


def checker_png(n=64, cells=8):
    """A tiny checkerboard PNG (texture creation belongs to the app layer)."""
    rows = b""
    for y in range(n):
        rows += b"\0" + b"".join(
            (b"\xff\xff\xff" if ((x * cells // n) + (y * cells // n)) % 2 else b"\x20\x40\xc0")
            for x in range(n))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def build():
    stage = miniusd.Stage(up_axis="Y", meters_per_unit=1.0)
    world = geom.add_xform(stage, "/World")
    world.kind = "assembly"

    # textured ground quad
    ground = geom.add_mesh(
        stage, "/World/Ground",
        points=[(-5, 0, -5), (5, 0, -5), (5, 0, 5), (-5, 0, 5)],
        face_vertex_counts=[4], face_vertex_indices=[0, 3, 2, 1],
        normals=[(0, 1, 0)] * 4,
        uvs=[(0, 0), (4, 0), (4, 4), (0, 4)])
    tex_mat = geom.add_preview_material(stage, "/World/Looks/Checker",
                                        diffuse_texture="textures/checker.png", roughness=0.8)
    geom.bind_material(ground, tex_mat)

    # a sphere mesh + a native cube, each with its own material
    pts, counts, idx, nrm = geom.uv_sphere_mesh(radius=1.0, segments=24, rings=12)
    ball = geom.add_mesh(stage, "/World/Ball", pts, counts, idx, normals=nrm)
    geom.set_transform(ball, translate=(0, 1, 0))
    red = geom.add_preview_material(stage, "/World/Looks/Red", diffuse_color=(0.8, 0.1, 0.1),
                                    roughness=0.3, metallic=0.0)
    geom.bind_material(ball, red)

    box = geom.add_cube(stage, "/World/Box", size=1.0, display_color=(0.2, 0.6, 0.2))
    geom.set_transform(box, translate=(2.5, 0.5, 0), rotate=(0, 30, 0))
    box.attribute("xformOp:rotateXYZ").set((0, 0, 0), time=1).set((0, 360, 0), time=48)
    stage.start_time_code, stage.end_time_code = 1, 48

    geom.add_camera(stage, "/World/Camera", translate=(0, 3, 12), rotate=(-12, 0, 0))
    geom.add_distant_light(stage, "/World/Sun", intensity=3.0, rotate=(-45, 30, 0))
    return stage


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    stage = build()
    png = checker_png()
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    with open(os.path.join(out, "textures", "checker.png"), "wb") as f:
        f.write(png)
    for ext in ("usda", "usdc"):
        print(stage.save(os.path.join(out, "scene." + ext)))
    print(stage.save(os.path.join(out, "scene.usdz"), assets={"textures/checker.png": png}))
