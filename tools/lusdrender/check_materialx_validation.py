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

    emission_source = out_dir / "constant-emission.mtlx"
    emission_source.write_text(
        '''<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="Emission" type="surfaceshader">
    <input name="base" type="float" value="0" />
    <input name="emission" type="float" value="1" />
    <input name="emission_color" type="color3" value="0.2, 0.4, 0.6" />
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
        raise RuntimeError(f"constant-emission render failed:\n{emission.stderr}")
    ew, eh, emission_rgba = read_png_rgba(emission_image)
    center = (eh // 2 * ew + ew // 2) * 4
    if (ew, eh) != (64, 64) or tuple(emission_rgba[center : center + 3]) != (124, 170, 203):
        actual = tuple(emission_rgba[center : center + 3])
        raise RuntimeError(f"constant emission disagrees with analytic sRGB result: {actual}")

    rejected = subprocess.run(
        [exe, "--mtlx", str(source), str(image), "--path-trace"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rejected.returncode == 0 or "CPU renderer only" not in rejected.stderr:
        raise RuntimeError("MaterialX validation accepted an unsupported GPU path")

    print("PASS: MaterialX preview sphere and constant-emission analytic check")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
