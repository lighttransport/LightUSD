#!/usr/bin/env python3
"""Check camera-selected preview refinement and settled headless captures."""
import argparse
import json
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageChops


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output / "cube.usda"
    source.write_text('#usda 1.0\ndef Cube "Cube" { double size=4 }\n')
    env = dict(os.environ, __NV_PRIME_RENDER_OFFLOAD="1",
               __GLX_VENDOR_LIBRARY_NAME="nvidia",
               LUSDVIEW_VBO_DEVLOCAL_MIN_KB="0",
               __EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/10_nvidia.json")
    results = []
    for kind in ("gaussian", "points", "volume"):
        for backend in ("cpu", "vk", "cuda"):
            reports = {}
            images = {}
            for pose, dolly in (("near", ".5"), ("far", "8"), ("static", ".5")):
                label = f"{kind}-{backend}-{pose}"
                report = args.output / (label + ".json")
                image = args.output / (label + ".ppm")
                cmd = [str(args.viewer.resolve()), "--headless", "--no-grid",
                       "--scene-preview", "--preview-kind", kind,
                       "--preview-resolution", "64", "--preview-budget-mib", "2048",
                       "--preview-cache-dir", str(args.output / "cache"),
                       "--cam-dolly", dolly, "--frames", "1", "--size", "320x240",
                       "--screenshot", str(image), "--render-report", str(report)]
                cmd += {"cpu": ["--cpu-rt"],
                        "vk": ["--backend", "vk", "--vk-device", "nvidia"],
                        "cuda": ["--cuda", "--cuda-rt-backend", "software"]}[backend]
                if pose == "static":
                    cmd += ["--preview-no-lod"]
                cmd += [str(source)]
                with (args.output / (label + ".log")).open("w") as log:
                    subprocess.run(cmd, stdout=log, stderr=log, env=env,
                                   timeout=180, check=True)
                text = (args.output / (label + ".log")).read_text()
                if "Validation Error" in text or "VUID-" in text:
                    raise RuntimeError(label + ": Vulkan validation failure")
                reports[pose] = json.loads(report.read_text())
                images[pose] = Image.open(image).convert("RGB")
                if pose != "static":
                    lod = reports[pose]["preview_lod"]
                    if lod["updates"] < 1:
                        raise RuntimeError(label + ": did not apply camera refinement")
                    if lod["selected_geometry_bytes"] > lod["budget_bytes"] - 512 * 1024**2:
                        raise RuntimeError(label + ": exceeded geometry budget")
            near, far = reports["near"]["preview_lod"], reports["far"]["preview_lod"]
            if not near["levels"][0] or near["selected_geometry_bytes"] <= far["selected_geometry_bytes"]:
                raise RuntimeError(f"{kind}-{backend}: close view did not refine")
            # Finest selection must preserve framing and content across the
            # asynchronous variant reload. Allow one 8-bit rounding step.
            delta = ImageChops.difference(images["near"], images["static"])
            maximum = max(upper for _, upper in delta.getextrema())
            if maximum > 1:
                raise RuntimeError(f"{kind}-{backend}: finest reload changed pixels ({maximum})")
            result = dict(kind=kind, backend=backend, near=near, far=far,
                          finest_max_pixel_difference=maximum)
            results.append(result)
            print(json.dumps(result), flush=True)
    (args.output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
