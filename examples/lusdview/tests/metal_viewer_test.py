#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise CLI selection, USD loading, Metal rendering and windowless UI capture."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def ppm(path):
    with path.open("rb") as f:
        assert f.readline().strip() == b"P6", "expected PPM screenshot"
        width, height = map(int, f.readline().split())
        assert int(f.readline()) == 255
        data = f.read()
    assert len(data) == width * height * 3, "truncated screenshot"
    return width, height, data


def main():
    binary, root = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="lusdview-metal-") as directory:
        tmp = Path(directory)
        config = tmp / "config.json"
        config.write_text('{}\n')
        # Include the macOS default and both explicit aliases. A missing backend
        # must fail instead of silently producing an OpenGL/Vulkan screenshot.
        for name, flags in (("default", []), ("metal", ["--backend", "metal"]),
                            ("mtl", ["--backend", "mtl"])):
            scene, ui, report = (tmp / (name + suffix)
                                 for suffix in (".ppm", "-ui.ppm", ".json"))
            result = subprocess.run(
                [str(binary), "--config", str(config), *flags, "--headless",
                 "--frames", "4", "--size", "640x480", "--screenshot", str(scene),
                 "--window-shot", str(ui), "--render-report", str(report),
                 str(root / "models/suzanne-pbr.usda")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=45)
            if result.returncode and "No native Metal device available" in result.stdout:
                print("SKIP: no Metal device visible")
                return 77
            assert result.returncode == 0, result.stdout
            info = json.loads(report.read_text())
            assert info["exit_code"] == 0 and info["backend"]["name"] == "Metal", info
            w, h, pixels = ppm(scene)
            assert w > 32 and h > 32, "missing viewport"
            colored = sum(max(pixels[i:i+3]) - min(pixels[i:i+3]) > 35
                          for i in range(0, len(pixels), 3))
            assert colored > w * h * .01, "missing shaded scene geometry"
            uw, uh, ui_pixels = ppm(ui)
            assert (uw, uh) == (640, 480), "incorrect headless composite size"
            assert len(set(ui_pixels)) > 40, "blank ImGui composite"
            print(f"PASS: {name}: Metal USD viewport {w}x{h}, UI {uw}x{uh}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
