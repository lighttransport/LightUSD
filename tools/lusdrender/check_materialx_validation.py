#!/usr/bin/env python3
"""Smoke-test the CPU MaterialX-to-preview validation entry point."""

import subprocess
import struct
import sys
from pathlib import Path

from check_lusdrender_smoke import read_png_rgba, write_png_rgba


def read_pfm_rgb(path: Path):
    data = path.read_bytes()
    lines = data.split(b"\n", 3)
    if len(lines) != 4 or lines[0] != b"PF":
        raise RuntimeError(f"{path} is not an RGB PFM image")
    width, height = (int(value) for value in lines[1].split())
    scale = float(lines[2])
    if scale >= 0.0:
        raise RuntimeError("validation PFM must use little-endian float data")
    payload = lines[3]
    expected = width * height * 3 * 4
    if len(payload) != expected:
        raise RuntimeError(f"{path} has {len(payload)} payload bytes; expected {expected}")
    return width, height, struct.unpack("<" + "f" * (width * height * 3), payload)


def main():
    if len(sys.argv) != 4:
        print("usage: check_materialx_validation.py <lusdrender> <repo> <outdir>", file=sys.stderr)
        return 2

    exe = sys.argv[1]
    root = Path(sys.argv[2])
    source = root / "data/materialx/StandardSurface/standard_surface_default.mtlx"
    out_dir = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    image = out_dir / "standard-surface.png"

    invalid_mips = subprocess.run(
        [exe, "--mtlx-mipmaps", "unused.usda", str(image)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if invalid_mips.returncode == 0 or "--mtlx-mipmaps requires --mtlx" not in invalid_mips.stderr:
        raise RuntimeError("MaterialX mip option was accepted without --mtlx")

    run = subprocess.run(
        [exe, "--mtlx", str(source), str(image), "-stats"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if run.returncode:
        raise RuntimeError(f"MaterialX validation render failed:\n{run.stderr}")
    if "MaterialX validation: sphere, uniform white environment, CPU LightRT BSDF" not in run.stderr:
        raise RuntimeError(f"validation render did not report its selected path:\n{run.stderr}")
    if "rt meshes: 1" not in run.stderr or "domelight: 1" not in run.stderr:
        raise RuntimeError(f"validation scene is missing its sphere or environment:\n{run.stderr}")

    width, height, rgba = read_png_rgba(image)
    if (width, height) != (512, 512):
        raise RuntimeError(f"unexpected validation image dimensions {(width, height)}")
    rgb = [rgba[i:i + 3] for i in range(0, len(rgba), 4)]
    if len(set(rgb)) <= 1:
        raise RuntimeError("MaterialX validation render appears blank")

    emission_source = out_dir / "connected-emission.mtlx"
    include_dir = out_dir / "connected-include"
    include_dir.mkdir(parents=True, exist_ok=True)
    (include_dir / "graph.mtlx").write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39" xmlns:xi="http://www.w3.org/2001/XInclude">
  <xi:include href="nodes.mtlx" />
</materialx>
''',
        encoding="utf-8",
    )
    (include_dir / "nodes.mtlx").write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39">
  <multiply name="EmissionColor" type="color3">
    <input name="in1" type="color3" value="0.4, 0.8, 1.2" />
    <input name="in2" type="color3" value="0.5, 0.5, 0.5" />
  </multiply>
</materialx>
''',
        encoding="utf-8",
    )
    emission_source.write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39" xmlns:xi="http://www.w3.org/2001/XInclude">
  <xi:include href="connected-include/graph.mtlx" />
  <standard_surface name="Emission" type="surfaceshader">
    <input name="base" type="float" value="0" />
    <input name="emission" type="float" value="1" />
    <input name="emission_color" type="color3" nodename="EmissionColor" />
  </standard_surface>
  <surfacematerial name="EmissionMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Emission" />
  </surfacematerial>
</materialx>
''',
        encoding="utf-8",
    )
    emission_image = out_dir / "constant-emission.png"
    emission = subprocess.run(
        [
            exe,
            "--mtlx",
            str(emission_source),
            str(emission_image),
            "--mtlx-material",
            "EmissionMaterial",
            "-w",
            "64",
            "-height",
            "64",
            "-diagnosticEmission",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if emission.returncode:
        raise RuntimeError(f"connected-emission render failed:\n{emission.stderr}")
    ew, eh, emission_rgba = read_png_rgba(emission_image)
    center = (eh // 2 * ew + ew // 2) * 4
    if (ew, eh) != (64, 64) or tuple(emission_rgba[center : center + 3]) != (124, 170, 203):
        actual = tuple(emission_rgba[center : center + 3])
        raise RuntimeError(f"connected emission disagrees with analytic sRGB result: {actual}")

    def render_diagnostic(source: Path, name: str, output: Path) -> bytes:
        run = subprocess.run(
            [
                exe,
                "--mtlx",
                str(source),
                str(output),
                "--mtlx-material",
                name,
                "-w",
                "64",
                "-height",
                "64",
                "-diagnosticEmission",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if run.returncode:
            raise RuntimeError(f"{name} validation render failed:\n{run.stderr}")
        width, height, rgba = read_png_rgba(output)
        if (width, height) != (64, 64):
            raise RuntimeError(f"{name} render has unexpected dimensions {(width, height)}")
        return rgba

    def require_spatial_variation(rgba: bytes, label: str) -> None:
        # Ignore the silhouette and background. A center-sampled material is
        # constant over this interior crop; only evaluating the graph at each
        # hit can produce the expected broad color range.
        pixels = [
            tuple(rgba[(y * 64 + x) * 4 : (y * 64 + x) * 4 + 3])
            for y in range(16, 48)
            for x in range(16, 48)
        ]
        visible = [pixel for pixel in pixels if max(pixel) > 16]
        if len(set(visible)) < 16:
            raise RuntimeError(f"{label} output is spatially constant inside the sphere")
        spread = [max(p[c] for p in visible) - min(p[c] for p in visible)
                  for c in range(3)]
        if max(spread) < 24:
            raise RuntimeError(f"{label} output has too little spatial variation: {spread}")

    position_source = out_dir / "position-emission.mtlx"
    position_source.write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39">
  <position name="Position" type="vector3" />
  <multiply name="HalfPosition" type="vector3">
    <input name="in1" type="vector3" nodename="Position" />
    <input name="in2" type="vector3" value="0.5, 0.5, 0.5" />
  </multiply>
  <add name="Position01" type="vector3">
    <input name="in1" type="vector3" nodename="HalfPosition" />
    <input name="in2" type="vector3" value="0.5, 0.5, 0.5" />
  </add>
  <convert name="PositionColor" type="color3">
    <input name="in" type="vector3" nodename="Position01" />
  </convert>
  <surface_unlit name="PositionEmission" type="surfaceshader">
    <input name="emission" type="float" value="1" />
    <input name="emission_color" type="color3" nodename="PositionColor" />
  </surface_unlit>
  <surfacematerial name="PositionMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="PositionEmission" />
  </surfacematerial>
</materialx>
''',
        encoding="utf-8",
    )
    position_image = out_dir / "position-emission.png"
    require_spatial_variation(
        render_diagnostic(position_source, "PositionMaterial", position_image),
        "position-driven emission",
    )

    position_aov = out_dir / "position-node-aov.pfm"
    node_aov = subprocess.run(
        [
            exe,
            "--mtlx",
            str(position_source),
            str(position_aov),
            "--mtlx-material",
            "PositionMaterial",
            "--aov",
            "mtlxNode:Position01",
            "-w",
            "64",
            "-height",
            "64",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if node_aov.returncode:
        raise RuntimeError(f"MaterialX node AOV render failed:\n{node_aov.stderr}")
    pw, ph, position_values = read_pfm_rgb(position_aov)
    if (pw, ph) != (64, 64):
        raise RuntimeError(f"MaterialX node AOV has unexpected dimensions {(pw, ph)}")
    position_pixels = [
        position_values[(y * pw + x) * 3 : (y * pw + x + 1) * 3]
        for y in range(16, 48)
        for x in range(16, 48)
    ]
    if len({tuple(round(c, 3) for c in pixel) for pixel in position_pixels}) < 16:
        raise RuntimeError("MaterialX node AOV is spatially constant inside the sphere")
    invalid_node_aov = subprocess.run(
        [
            exe,
            "--mtlx",
            str(position_source),
            str(out_dir / "missing-node-aov.pfm"),
            "--mtlx-material",
            "PositionMaterial",
            "--aov",
            "mtlxNode:MissingNode",
            "-w",
            "64",
            "-height",
            "64",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if invalid_node_aov.returncode == 0 or "MaterialX node AOV is invalid" not in invalid_node_aov.stderr:
        raise RuntimeError("MaterialX node AOV accepted a missing node")

    texture_path = out_dir / "gradient.png"
    gradient = bytearray()
    for y in range(16):
        for x in range(16):
            gradient.extend((x * 17, y * 17, 255 - x * 17, 255))
    write_png_rgba(texture_path, 16, 16, gradient)
    texture_source = out_dir / "texture-emission.mtlx"
    texture_source.write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39">
  <image name="Gradient" type="color3">
    <input name="file" type="filename" value="gradient.png" />
  </image>
  <surface_unlit name="TextureEmission" type="surfaceshader">
    <input name="emission" type="float" value="1" />
    <input name="emission_color" type="color3" nodename="Gradient" />
  </surface_unlit>
  <surfacematerial name="TextureMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="TextureEmission" />
  </surfacematerial>
</materialx>
''',
        encoding="utf-8",
    )
    texture_image = out_dir / "texture-emission.png"
    require_spatial_variation(
        render_diagnostic(texture_source, "TextureMaterial", texture_image),
        "UV-driven image emission",
    )

    checker = bytearray()
    for y in range(288):
        for x in range(288):
            value = 255 if (x + y) % 2 else 0
            checker.extend((value, value, value, 255))
    write_png_rgba(out_dir / "checker.png", 288, 288, checker)
    checker_source = out_dir / "checker.mtlx"
    checker_source.write_text(
        texture_source.read_text(encoding="utf-8").replace("gradient.png", "checker.png"),
        encoding="utf-8",
    )
    rendered = []
    for enabled in (False, True):
        result_path = out_dir / ("checker-mip.pfm" if enabled else "checker-base.pfm")
        command = [exe, "--mtlx", str(checker_source), str(result_path),
                   "--mtlx-material", "TextureMaterial", "--aov", "mtlxNode:Gradient",
                   "-w", "64", "-height", "64"]
        if enabled:
            command.extend(("--mtlx-mipmaps", "-texBudgetMb", "3"))
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        if result.returncode:
            raise RuntimeError(f"MaterialX checker render failed:\n{result.stderr}")
        _, _, values = read_pfm_rgb(result_path)
        rendered.append([values[(y * 64 + x) * 3] for y in range(28, 36)
                         for x in range(28, 36)])
    if max(abs(v - 0.5) for v in rendered[1]) > 0.02:
        raise RuntimeError("MaterialX mip filtering did not converge to the checker mean")
    if max(abs(a - b) for a, b in zip(*rendered)) < 0.001:
        raise RuntimeError("MaterialX mip option did not affect minified texture sampling")
    tight_budget = subprocess.run(
        [exe, "--mtlx", str(checker_source), str(out_dir / "budget.pfm"),
         "--mtlx-material", "TextureMaterial", "--mtlx-mipmaps",
         "-texBudgetMb", "2", "-w", "64", "-height", "64"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if tight_budget.returncode == 0 or "within budget" not in tight_budget.stderr:
        raise RuntimeError(f"MaterialX mip budget did not reject the chain:\n{tight_budget.stderr}")

    lobes_source = out_dir / "lobe-weights.mtlx"
    lobes_source.write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="Lobes" type="surfaceshader">
    <input name="base_weight" type="float" value="1" />
    <input name="base_color" type="color3" value="0.3, 0.5, 0.7" />
    <input name="base_metalness" type="float" value="0.15" />
    <input name="specular_weight" type="float" value="0.75" />
    <input name="specular_roughness" type="float" value="0.35" />
    <input name="transmission_weight" type="float" value="0.2" />
    <input name="subsurface_weight" type="float" value="0.25" />
    <input name="coat_weight" type="float" value="0.4" />
    <input name="sheen_weight" type="float" value="0.3" />
  </open_pbr_surface>
  <surfacematerial name="LobeMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Lobes" />
  </surfacematerial>
</materialx>
''',
        encoding="utf-8",
    )
    lobe_aovs = (
        "mtlxDiffuseWeight",
        "mtlxSpecularWeight",
        "mtlxCoatWeight",
        "mtlxTransmissionWeight",
    )
    lobe_values = []
    for aov in lobe_aovs:
        lobe_image = out_dir / f"{aov}.pfm"
        lobe_render = subprocess.run(
            [
                exe,
                "--mtlx",
                str(lobes_source),
                str(lobe_image),
                "--mtlx-material",
                "LobeMaterial",
                "--aov",
                aov,
                "-w",
                "64",
                "-height",
                "64",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if lobe_render.returncode:
            raise RuntimeError(f"{aov} render failed:\n{lobe_render.stderr}")
        lw, lh, values = read_pfm_rgb(lobe_image)
        if (lw, lh) != (64, 64):
            raise RuntimeError(f"{aov} AOV has unexpected dimensions {(lw, lh)}")
        center = (lh // 2 * lw + lw // 2) * 3
        value = values[center]
        if value <= 0.0:
            raise RuntimeError(f"{aov} AOV is not positive at the sphere center: {value}")
        lobe_values.append(value)
    if not 0.98 <= sum(lobe_values) <= 1.02:
        raise RuntimeError(f"MaterialX lobe selection weights do not sum to 1: {lobe_values}")

    lambert_source = out_dir / "lambert.mtlx"
    lambert_source.write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="Lambert" type="surfaceshader">
    <input name="base_weight" type="float" value="1" />
    <input name="base_color" type="color3" value="0.2, 0.4, 0.6" />
    <input name="base_roughness" type="float" value="1" />
    <input name="metalness" type="float" value="0" />
    <input name="specular_weight" type="float" value="0" />
    <input name="transmission_weight" type="float" value="0" />
    <input name="subsurface_weight" type="float" value="0" />
    <input name="coat_weight" type="float" value="0" />
    <input name="sheen_weight" type="float" value="0" />
  </open_pbr_surface>
  <surfacematerial name="LambertMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Lambert" />
  </surfacematerial>
</materialx>
''',
        encoding="utf-8",
    )
    lambert_image = out_dir / "lambert.png"
    lambert = subprocess.run(
        [
            exe,
            "--mtlx",
            str(lambert_source),
            str(lambert_image),
            "--mtlx-material",
            "LambertMaterial",
            "-w",
            "64",
            "-height",
            "64",
            "--displayTransform",
            "srgb",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if lambert.returncode:
        raise RuntimeError(f"Lambert validation render failed:\n{lambert.stderr}")
    lw, lh, lambert_rgba = read_png_rgba(lambert_image)
    center = (lh // 2 * lw + lw // 2) * 4
    actual = tuple(lambert_rgba[center : center + 3])
    expected = (124, 170, 203)
    if (lw, lh) != (64, 64) or any(
        abs(actual[channel] - expected[channel]) > 3 for channel in range(3)
    ):
        raise RuntimeError(
            f"uniform-environment Lambert disagrees with analytic sRGB result: {actual}"
        )

    dark_source = out_dir / "no-scattering.mtlx"
    dark_source.write_text(
        lambert_source.read_text(encoding="utf-8").replace(
            '<input name="base_weight" type="float" value="1" />',
            '<input name="base_weight" type="float" value="0" />',
        ), encoding="utf-8",
    )
    dark_image = out_dir / "no-scattering.png"
    dark = subprocess.run(
        [exe, "--mtlx", str(dark_source), str(dark_image), "--mtlx-material",
         "LambertMaterial", "-w", "64", "-height", "64", "--displayTransform", "srgb"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if dark.returncode:
        raise RuntimeError(f"disabled-scattering render failed:\n{dark.stderr}")
    _, _, dark_rgba = read_png_rgba(dark_image)
    for x, y in ((32, 32), (36, 32), (32, 36)):
        offset = (y * 64 + x) * 4
        if max(dark_rgba[offset : offset + 3]) > 2:
            raise RuntimeError("disabled specular produced environment reflection")

    rough_source = out_dir / "rough-diffuse.mtlx"
    rough_source.write_text(
        lambert_source.read_text(encoding="utf-8").replace(
            '<input name="base_weight" type="float" value="1" />',
            '<input name="base_weight" type="float" value="1" />'
            '<input name="base_diffuse_roughness" type="float" value="1" />',
        ),
        encoding="utf-8",
    )
    rough_image = out_dir / "rough-diffuse.png"
    rough = subprocess.run(
        [exe, "--mtlx", str(rough_source), str(rough_image),
         "--mtlx-material", "LambertMaterial", "-w", "64", "-height", "64",
         "--displayTransform", "srgb"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if rough.returncode:
        raise RuntimeError(f"rough-diffuse render failed:\n{rough.stderr}")
    rw, rh, rough_rgba = read_png_rgba(rough_image)
    if (rw, rh) != (lw, lh):
        raise RuntimeError("rough diffuse render dimensions changed")
    # Compare sphere pixels, not the background. A nonzero roughness must reach
    # the actual per-hit CPU shading path and change its angular response.
    differences = [abs(rough_rgba[i] - lambert_rgba[i])
                   for i in range(0, len(rough_rgba), 4)]
    if max(differences) < 2:
        raise RuntimeError("OpenPBR diffuse roughness did not affect the render")

    # Standard Surface uses qualitative Oren–Nayar instead of OpenPBR's EON.
    standard_images = []
    for roughness in (0, 1):
        standard_source = out_dir / f"standard-diffuse-{roughness}.mtlx"
        standard_source.write_text(f'''<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="s" type="surfaceshader">
    <input name="base" type="float" value="1" />
    <input name="base_color" type="color3" value="0.2, 0.4, 0.6" />
    <input name="diffuse_roughness" type="float" value="{roughness}" />
    <input name="specular" type="float" value="0" />
  </standard_surface>
  <surfacematerial name="m" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="s" />
  </surfacematerial>
</materialx>
''', encoding="utf-8")
        standard_image = out_dir / f"standard-diffuse-{roughness}.png"
        standard = subprocess.run(
            [exe, "--mtlx", str(standard_source), str(standard_image),
             "--mtlx-material", "m", "-w", "64", "-height", "64",
             "--displayTransform", "srgb"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if standard.returncode:
            raise RuntimeError(f"standard rough diffuse render failed:\n{standard.stderr}")
        sw, sh, pixels = read_png_rgba(standard_image)
        if (sw, sh) != (lw, lh):
            raise RuntimeError("standard diffuse render dimensions changed")
        standard_images.append(pixels)
    if max(abs(standard_images[0][i] - standard_images[1][i])
           for i in range(0, len(standard_images[0]), 4)) < 2:
        raise RuntimeError("standard-surface diffuse roughness did not affect the render")

    # A graph-level mix of different diffuse closures must survive the
    # MaterialX runtime bridge. Compare linearized colors with separately
    # rendered closures; all renders use identical geometry and lighting.
    closure_parts = '''<oren_nayar_diffuse_bsdf name="a" type="BSDF">
      <input name="color" type="color3" value="0.2, 0.5, 0.8"/>
      <input name="roughness" type="float" value="0.9"/>
    </oren_nayar_diffuse_bsdf>
    <burley_diffuse_bsdf name="b" type="BSDF">
      <input name="color" type="color3" value="0.8, 0.3, 0.1"/>
      <input name="roughness" type="float" value="0.4"/>
    </burley_diffuse_bsdf>
    <oren_nayar_diffuse_bsdf name="top" type="BSDF">
      <input name="weight" type="float" value="0.25"/>
      <input name="color" type="color3" value="0.2, 0.5, 0.8"/>
      <input name="roughness" type="float" value="0.9"/>
    </oren_nayar_diffuse_bsdf>
    <layer name="layered" type="BSDF">
      <input name="top" type="BSDF" nodename="top"/>
      <input name="base" type="BSDF" nodename="b"/>
    </layer>
    <mix name="mix" type="BSDF">
      <input name="bg" type="BSDF" nodename="a"/>
      <input name="fg" type="BSDF" nodename="b"/>
      <input name="mix" type="float" value="0.25"/>
    </mix>
    <constant name="factor" type="color3">
      <input name="value" type="color3" value="0.2, 0.5, 0.9"/>
    </constant>
    <multiply name="tint" type="BSDF">
      <input name="in1" type="BSDF" nodename="mix"/>
      <input name="in2" type="color3" nodename="factor"/>
    </multiply>
    <conductor_bsdf name="metal" type="BSDF">
      <input name="weight" type="float" value="0.4"/>
      <input name="roughness" type="vector2" value="0.5, 0.5"/>
    </conductor_bsdf>
    <conductor_bsdf name="metal_film_a" type="BSDF">
      <input name="weight" type="float" value="0.4"/>
      <input name="roughness" type="vector2" value="0.5, 0.5"/>
      <input name="thinfilm_thickness" type="float" value="400"/>
      <input name="thinfilm_ior" type="float" value="1.5"/>
    </conductor_bsdf>
    <conductor_bsdf name="metal_film_b" type="BSDF">
      <input name="weight" type="float" value="0.4"/>
      <input name="roughness" type="vector2" value="0.5, 0.5"/>
      <input name="thinfilm_thickness" type="float" value="800"/>
      <input name="thinfilm_ior" type="float" value="1.25"/>
    </conductor_bsdf>
    <mix name="metal_film_mix" type="BSDF">
      <input name="bg" type="BSDF" nodename="metal_film_a"/>
      <input name="fg" type="BSDF" nodename="metal_film_b"/>
      <input name="mix" type="float" value="0.25"/>
    </mix>
    <conductor_bsdf name="metal_normal_a" type="BSDF">
      <input name="weight" type="float" value="0.4"/>
      <input name="roughness" type="vector2" value="0.5, 0.5"/>
      <input name="normal" type="vector3" value="0.6, 0, 0.8"/>
    </conductor_bsdf>
    <conductor_bsdf name="metal_normal_b" type="BSDF">
      <input name="weight" type="float" value="0.4"/>
      <input name="roughness" type="vector2" value="0.5, 0.5"/>
      <input name="normal" type="vector3" value="0, 0, 1"/>
    </conductor_bsdf>
    <mix name="metal_normal_mix" type="BSDF">
      <input name="bg" type="BSDF" nodename="metal_normal_a"/>
      <input name="fg" type="BSDF" nodename="metal_normal_b"/>
      <input name="mix" type="float" value="0.25"/>
    </mix>
    <dielectric_bsdf name="spec_dielectric" type="BSDF">
      <input name="ior" type="float" value="1.6"/>
      <input name="tint" type="color3" value="0.5,0.8,1"/>
      <input name="roughness" type="vector2" value="0.2,0.7"/>
    </dielectric_bsdf>
    <generalized_schlick_bsdf name="spec_schlick" type="BSDF">
      <input name="color0" type="color3" value="0.1,0.35,0.7"/>
      <input name="color82" type="color3" value="0.7,0.9,0.3"/>
      <input name="color90" type="color3" value="0.9,0.8,0.6"/>
      <input name="exponent" type="float" value="4"/>
      <input name="roughness" type="vector2" value="0.2,0.7"/>
      <input name="thinfilm_thickness" type="float" value="500"/>
      <input name="thinfilm_ior" type="float" value="1.4"/>
    </generalized_schlick_bsdf>
    <mix name="spec_mix" type="BSDF">
      <input name="bg" type="BSDF" nodename="spec_dielectric"/>
      <input name="fg" type="BSDF" nodename="spec_schlick"/>
      <input name="mix" type="float" value="0.3"/>
    </mix>
    <mix name="metal_mix" type="BSDF">
      <input name="bg" type="BSDF" nodename="a"/>
      <input name="fg" type="BSDF" nodename="metal"/>
      <input name="mix" type="float" value="0.25"/>
    </mix>
    <layer name="metal_layer" type="BSDF">
      <input name="top" type="BSDF" nodename="metal"/>
      <input name="base" type="BSDF" nodename="a"/>
    </layer>
    <multiply name="metal_tint" type="BSDF">
      <input name="in1" type="BSDF" nodename="metal_mix"/>
      <input name="in2" type="color3" nodename="factor"/>
    </multiply>
    <sheen_bsdf name="black_sheen" type="BSDF">
      <input name="color" type="color3" value="0, 0, 0"/>
      <input name="roughness" type="float" value="0.8"/>
    </sheen_bsdf>
    <layer name="sheen_layer" type="BSDF">
      <input name="top" type="BSDF" nodename="black_sheen"/>
      <input name="base" type="BSDF" nodename="a"/>
    </layer>
    <sheen_bsdf name="sheen_a" type="BSDF">
      <input name="color" type="color3" value="0.2, 0.5, 0.8"/>
      <input name="roughness" type="float" value="0.2"/>
    </sheen_bsdf>
    <sheen_bsdf name="sheen_b" type="BSDF">
      <input name="color" type="color3" value="0.8, 0.3, 0.1"/>
      <input name="roughness" type="float" value="0.8"/>
      <input name="mode" type="string" value="zeltner"/>
    </sheen_bsdf>
    <sheen_bsdf name="sheen_b_conty" type="BSDF">
      <input name="color" type="color3" value="0.8, 0.3, 0.1"/>
      <input name="roughness" type="float" value="0.8"/>
    </sheen_bsdf>
    <mix name="sheen_mix" type="BSDF">
      <input name="bg" type="BSDF" nodename="sheen_a"/>
      <input name="fg" type="BSDF" nodename="sheen_b"/>
      <input name="mix" type="float" value="0.4"/>
    </mix>
    <multiply name="sheen_tint" type="BSDF">
      <input name="in1" type="BSDF" nodename="sheen_mix"/>
      <input name="in2" type="color3" nodename="factor"/>
    </multiply>'''
    closure_images = []
    for root_name in ("a", "b", "mix", "tint", "sheen_a", "sheen_b", "sheen_mix", "sheen_tint", "sheen_b_conty", "layered", "sheen_layer", "metal", "metal_mix", "metal_layer", "metal_tint", "metal_film_a", "metal_film_b", "metal_film_mix", "metal_normal_a", "metal_normal_b", "metal_normal_mix", "spec_dielectric", "spec_schlick", "spec_mix"):
        closure_source = out_dir / f"closure-{root_name}.mtlx"
        closure_source.write_text(
            f'''<materialx version="1.39">{closure_parts}
              <surface name="s" type="surfaceshader">
                <input name="bsdf" type="BSDF" nodename="{root_name}"/>
              </surface>
              <surfacematerial name="m" type="material">
                <input name="surfaceshader" type="surfaceshader" nodename="s"/>
              </surfacematerial>
            </materialx>''', encoding="utf-8")
        closure_image = out_dir / f"closure-{root_name}.png"
        closure = subprocess.run(
            [exe, "--mtlx", str(closure_source), str(closure_image),
             "--mtlx-material", "m", "-w", "64", "-height", "64",
             "--displayTransform", "srgb"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if closure.returncode:
            raise RuntimeError(f"diffuse closure {root_name} render failed:\n{closure.stderr}")
        cw, ch, pixels = read_png_rgba(closure_image)
        if (cw, ch) != (64, 64):
            raise RuntimeError("diffuse closure render dimensions changed")
        closure_images.append(pixels)

    def linear_srgb(byte):
        value = byte / 255.0
        return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4

    center_offset = (32 * 64 + 32) * 4
    if max(abs(closure_images[0][center_offset + c] - closure_images[1][center_offset + c])
           for c in range(3)) < 20:
        raise RuntimeError("diffuse closure component renders are not distinguishable")
    for x, y in ((32, 32), (30, 30), (34, 30), (30, 34)):
        offset = (y * 64 + x) * 4
        for channel in range(3):
            a, b, mixed, tinted = (linear_srgb(p[offset + channel]) for p in closure_images[:4])
            if abs(tinted - mixed * (0.2, 0.5, 0.9)[channel]) > 0.025:
                raise RuntimeError(
                    f"diffuse color scaling failed at {(x, y, channel)}: {tinted}"
                )
            layer = linear_srgb(closure_images[9][offset + channel])
            if abs(layer - (0.25 * a + 0.75 * b)) > 0.025:
                raise RuntimeError(f"opaque layer throughput failed at {(x, y, channel)}")
            if abs(mixed - (0.75 * a + 0.25 * b)) > 0.025:
                raise RuntimeError(
                    f"diffuse mixture did not preserve weighted response at {(x, y, channel)}: "
                    f"{mixed} vs {0.75 * a + 0.25 * b}"
                )

    if max(abs(closure_images[4][center_offset + c] - closure_images[5][center_offset + c])
           for c in range(3)) < 2:
        raise RuntimeError("sheen component renders are not distinguishable")
    for x, y in ((32, 32), (30, 30), (34, 30), (30, 34)):
        offset = (y * 64 + x) * 4
        for channel in range(3):
            a, b, mixed, tinted = (linear_srgb(p[offset + channel]) for p in closure_images[4:8])
            if abs(mixed - (0.6 * a + 0.4 * b)) > 0.025 or \
               abs(tinted - mixed * (0.2, 0.5, 0.9)[channel]) > 0.025:
                raise RuntimeError(f"sheen mixture or RGB scaling failed at {(x, y, channel)}")

    if max(abs(closure_images[5][center_offset + c] - closure_images[8][center_offset + c])
           for c in range(3)) < 2:
        raise RuntimeError("authored Zeltner and Conty–Kulla modes rendered identically")

    # A black sheen top emits no visible sheen but still attenuates the base.
    # Compare the full image to avoid assuming a particular sphere pixel layout.
    if max(closure_images[0][i] - closure_images[10][i]
           for i in range(0, len(closure_images[0]), 4)) < 3:
        raise RuntimeError("view-dependent sheen layer did not attenuate its base")

    for x, y in ((32, 32), (30, 30), (34, 30), (30, 34)):
        offset = (y * 64 + x) * 4
        for channel in range(3):
            base, metal, mixed, layer, tinted = (linear_srgb(closure_images[i][offset + channel])
                                                   for i in (0, 11, 12, 13, 14))
            if abs(mixed - (0.75 * base + 0.25 * metal)) > 0.025 or \
               abs(layer - (0.6 * base + metal)) > 0.025 or \
               abs(tinted - mixed * (0.2, 0.5, 0.9)[channel]) > 0.025:
                raise RuntimeError(f"conductor mixture or layer failed at {(x, y, channel)}")

    if max(abs(closure_images[15][i] - closure_images[16][i])
           for i in range(len(closure_images[15])) if i % 4 != 3) < 2:
        raise RuntimeError("conductor films rendered identically")
    for x, y in ((32, 32), (30, 30), (34, 30), (30, 34)):
        offset = (y * 64 + x) * 4
        for channel in range(3):
            a, b, mixed = (linear_srgb(closure_images[i][offset + channel])
                           for i in (15, 16, 17))
            if abs(mixed - (0.75 * a + 0.25 * b)) > 0.025:
                raise RuntimeError(f"conductor film mixture failed at {(x, y, channel)}")

    if max(abs(closure_images[18][i] - closure_images[19][i])
           for i in range(len(closure_images[18])) if i % 4 != 3) < 2:
        raise RuntimeError("conductor normals rendered identically")
    for x, y in ((32, 32), (30, 30), (34, 30), (30, 34)):
        offset = (y * 64 + x) * 4
        for channel in range(3):
            a, b, mixed = (linear_srgb(closure_images[i][offset + channel])
                           for i in (18, 19, 20))
            if abs(mixed - (0.75 * a + 0.25 * b)) > 0.025:
                raise RuntimeError(f"conductor normal mixture failed at {(x, y, channel)}")

    if max(abs(closure_images[21][i] - closure_images[22][i])
           for i in range(len(closure_images[21])) if i % 4 != 3) < 2:
        raise RuntimeError("specular closures rendered identically")
    for x, y in ((32, 32), (30, 30), (34, 30), (30, 34)):
        offset = (y * 64 + x) * 4
        for channel in range(3):
            a, b, mixed = (linear_srgb(closure_images[i][offset + channel])
                           for i in (21, 22, 23))
            if abs(mixed - (0.7 * a + 0.3 * b)) > 0.025:
                raise RuntimeError(f"specular closure mixture failed at {(x, y, channel)}")

    # The CPU path tracer must converge to Lambert under the white environment
    # and produce scene-linear, deterministic PFM output across worker counts.
    path_source = out_dir / "path-lambert.mtlx"
    path_source.write_text('''<materialx version="1.39">
      <diffuse_bsdf name="d" type="BSDF"><input name="color" type="color3" value="0.2,0.4,0.6"/></diffuse_bsdf>
      <surface name="s" type="surfaceshader"><input name="bsdf" type="BSDF" nodename="d"/></surface>
      <surfacematerial name="m" type="material"><input name="surfaceshader" type="surfaceshader" nodename="s"/></surfacematerial>
    </materialx>''', encoding="utf-8")
    path_images = []
    for seed, threads in ((17, 1), (17, 4), (18, 4)):
        path_image = out_dir / f"path-lambert-{seed}-{threads}.pfm"
        run_path = subprocess.run(
            [exe, "--mtlx", str(path_source), str(path_image), "--mtlx-material", "m",
             "-w", "32", "-height", "32", "--path-trace", "--pt-samples", "2048",
             "--pt-max-depth", "1", "--pt-seed", str(seed), "-threads", str(threads)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run_path.returncode:
            raise RuntimeError(f"CPU MaterialX path trace failed:\n{run_path.stderr}")
        pw, ph, pixels = read_pfm_rgb(path_image)
        if (pw, ph) != (32, 32):
            raise RuntimeError("CPU path trace dimensions changed")
        center = (16 * 32 + 16) * 3
        if any(abs(pixels[center+i] - expected) > 0.025
               for i, expected in enumerate((0.2, 0.4, 0.6))):
            raise RuntimeError(f"CPU path Lambert response differs: {pixels[center:center+3]}")
        path_images.append(path_image.read_bytes())
    if path_images[0] != path_images[1]:
        raise RuntimeError("CPU MaterialX path trace changed across worker counts")
    if path_images[0] == path_images[2]:
        raise RuntimeError("CPU MaterialX path trace ignored its seed")

    path_emission_source = out_dir / "path-emission.mtlx"
    path_emission_source.write_text(
        emission_source.read_text(encoding="utf-8").replace(
            '<input name="base" type="float" value="0" />',
            '<input name="base" type="float" value="0" />\n'
            '    <input name="specular" type="float" value="0" />'),
        encoding="utf-8")
    path_emission = out_dir / "path-emission.pfm"
    emitted = subprocess.run(
        [exe, "--mtlx", str(path_emission_source), str(path_emission), "--mtlx-material", "EmissionMaterial",
         "-w", "32", "-height", "32", "--path-trace", "--pt-samples", "8", "--pt-max-depth", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if emitted.returncode:
        raise RuntimeError(f"CPU path emission failed:\n{emitted.stderr}")
    ew, eh, emission_pixels = read_pfm_rgb(path_emission)
    center = (16 * ew + 16) * 3
    if (ew, eh) != (32, 32) or any(abs(emission_pixels[center+i] - expected) > 2e-5
                                    for i, expected in enumerate((0.2, 0.4, 0.6))):
        raise RuntimeError(f"CPU path emission differs: {emission_pixels[center:center+3]}")

    # A transparent sphere requires two surface interactions before the ray
    # reaches the environment. This also checks visibility of its back faces.
    transmission_source = out_dir / "path-transmission.mtlx"
    transmission_source.write_text('''<materialx version="1.39">
      <dielectric_bsdf name="d" type="BSDF"><input name="ior" type="float" value="1"/>
        <input name="scatter_mode" type="string" value="T"/>
        <input name="roughness" type="vector2" value="0,0"/></dielectric_bsdf>
      <surface name="s" type="surfaceshader"><input name="bsdf" type="BSDF" nodename="d"/></surface>
      <surfacematerial name="m" type="material"><input name="surfaceshader" type="surfaceshader" nodename="s"/></surfacematerial>
    </materialx>''', encoding="utf-8")
    for depth, expected in ((1, 0.0), (3, 1.0)):
        path_image = out_dir / f"path-transmission-{depth}.pfm"
        run_path = subprocess.run(
            [exe, "--mtlx", str(transmission_source), str(path_image), "--mtlx-material", "m",
             "-w", "32", "-height", "32", "--path-trace", "--pt-samples", "256",
             "--pt-max-depth", str(depth)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run_path.returncode:
            raise RuntimeError(f"CPU transmission path failed:\n{run_path.stderr}")
        _, _, pixels = read_pfm_rgb(path_image)
        center = (16 * 32 + 16) * 3
        if any(abs(pixels[center+i] - expected) > 0.08 for i in range(3)):
            raise RuntimeError(f"CPU transmission depth {depth} differs: {pixels[center:center+3]}")

    # The authored OpenPBR coat-darkening amount must survive graph evaluation
    # and the LightRT bridge all the way to the path-traced color.
    coat_colors = []
    for darkening in (0, 1):
        coat_source = out_dir / f"path-coat-{darkening}.mtlx"
        coat_source.write_text(f'''<materialx version="1.39">
          <constant name="dark" type="float"><input name="value" type="float" value="{darkening}"/></constant>
          <open_pbr_surface name="s" type="surfaceshader">
            <input name="base_color" type="color3" value="0.2,0.4,0.6"/>
            <input name="coat_weight" type="float" value="0.8"/>
            <input name="coat_roughness" type="float" value="0.2"/>
            <input name="coat_darkening" type="float" nodename="dark"/>
          </open_pbr_surface>
          <surfacematerial name="m" type="material"><input name="surfaceshader" type="surfaceshader" nodename="s"/></surfacematerial>
        </materialx>''', encoding="utf-8")
        coat_image = out_dir / f"path-coat-{darkening}.pfm"
        rendered = subprocess.run(
            [exe, "--mtlx", str(coat_source), str(coat_image), "--mtlx-material", "m",
             "-w", "32", "-height", "32", "--path-trace", "--pt-samples", "1024",
             "--pt-max-depth", "1", "--pt-seed", "1"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if rendered.returncode:
            raise RuntimeError(f"CPU coat path failed:\n{rendered.stderr}")
        _, _, pixels = read_pfm_rgb(coat_image)
        center = (16 * 32 + 16) * 3
        coat_colors.append(pixels[center:center+3])
    if any(coat_colors[0][i] - coat_colors[1][i] < 0.025 for i in range(3)):
        raise RuntimeError(f"OpenPBR coat darkening did not reach the rendered BSDF: {coat_colors}")

    rejected = subprocess.run(
        [exe, "--mtlx", str(source), str(image), "--path-trace", "-cuda"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rejected.returncode == 0 or "CPU renderer only" not in rejected.stderr:
        raise RuntimeError("MaterialX validation accepted an unsupported GPU path")

    print("PASS: MaterialX graph, spatial/UV evaluation, node/lobe AOVs, Lambert and rough diffuse checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
