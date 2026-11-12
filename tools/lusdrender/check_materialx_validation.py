#!/usr/bin/env python3
"""Smoke-test the CPU MaterialX-to-preview validation entry point."""

import subprocess
import sys
from pathlib import Path

from check_lusdrender_smoke import read_png_rgba


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

    rejected = subprocess.run(
        [exe, "--mtlx", str(source), str(image), "--path-trace"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rejected.returncode == 0 or "CPU renderer only" not in rejected.stderr:
        raise RuntimeError("MaterialX validation accepted an unsupported GPU path")

    print("PASS: MaterialX graph, emission, and uniform-environment Lambert checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
