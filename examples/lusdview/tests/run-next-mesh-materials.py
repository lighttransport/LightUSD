#!/usr/bin/env python3
"""Check prototype geometry, materials, cycles, subsets, and instance budgets."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def mesh(name, translate=""):
    return f'''def Mesh "{name}" {{
        point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
        int[] faceVertexCounts = [3,3]
        int[] faceVertexIndices = [0,1,2,0,2,3]
        uniform token subdivisionScheme = "none"
        bool doubleSided = true
        texCoord2f[] primvars:st = [(0.25,0.5),(0.25,0.5),(0.25,0.5),(0.25,0.5),(0.25,0.5),(0.25,0.5)] (interpolation = "faceVarying")
        texCoord2f[] primvars:st1 = [(0.75,0.5),(0.75,0.5),(0.75,0.5),(0.75,0.5)] (interpolation = "vertex")
        rel material:binding = </World/Preview>
        rel material:binding:preview = </World/Preview>
        rel material:binding:full = </World/Full>
        {translate}
    }}'''


def materials(root):
    return f'''
    def Material "Preview" {{
        token outputs:surface.connect = </World/Preview/Surface.outputs:surface>
        def Shader "Surface" {{
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Preview/Texture.outputs:rgb>
            token outputs:surface
        }}
        def Shader "Texture" {{
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @uv.ppm@
            token inputs:sourceColorSpace = "raw"
            float2 inputs:st.connect = </World/Preview/Reader.outputs:result>
            float3 outputs:rgb
        }}
        def Shader "Reader" {{
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st1"
            float2 outputs:result
        }}
    }}
    def Material "Full" {{
        token outputs:surface.connect = </World/Full/Surface.outputs:surface>
        def Shader "Surface" {{
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0,0,1)
            token outputs:surface
        }}
    }}
'''.replace("/World", root)


def fixture(kind):
    ordinary = mesh("Ordinary", '''double3 xformOp:translate = (-1.5,0,0)
        uniform token[] xformOpOrder = ["xformOp:translate"]''')
    if kind == "point":
        prototype = mesh("Quad")
        instances = f'''def Scope "Prototypes" {{ {prototype} }}
        def PointInstancer "Copies" {{
            rel prototypes = [</World/Prototypes/Quad>]
            int[] protoIndices = [0]
            point3f[] positions = [(1.5,0,0)]
        }}'''
    else:
        instances = ""
        for name, position in (("CopyA", "(0,0,0)"), ("CopyB", "(1.5,0,0)")):
            instances += f'''def Xform "{name}" (
                instanceable = true
                prepend references = @prototype.usda@</Proto>
            ) {{
                double3 xformOp:translate = {position}
                uniform token[] xformOpOrder = ["xformOp:translate"]
            }}'''
    return f'''#usda 1.0
(defaultPrim = "World" upAxis = "Y")
def Xform "World" {{
    {ordinary}
    {instances}
    def Camera "Camera" {{
        double3 xformOp:translate = (0,0,10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        float focalLength = 30
    }}
    {materials("/World")}
}}
'''


def read_ppm(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise RuntimeError("invalid PPM capture")
    width, height = map(int, header.groups())
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated PPM capture")
    return width, height, pixels


def sanitized_fixture():
    materials = ""
    for name, color in (("Whole", "(0,0,1)"), ("Left", "(1,0,0)"), ("Right", "(0,1,0)")):
        materials += f'''def Material "{name}" {{
            token outputs:surface.connect = </World/{name}/Surface.outputs:surface>
            def Shader "Surface" {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = {color}
                token outputs:surface
            }}
        }}'''
    return f'''#usda 1.0
def Xform "World" {{
    def Mesh "Panels" {{
        point3f[] points = [(-2.5,-1,0),(-0.5,-1,0),(-0.5,1,0),(-2.5,1,0),
                           (0.5,-1,0),(2.5,-1,0),(2.5,1,0),(0.5,1,0)]
        int[] faceVertexCounts = [3,4,4]
        int[] faceVertexIndices = [0,1,99,0,1,2,3,4,5,6,7]
        uniform token subdivisionScheme = "none"
        bool doubleSided = true
        rel material:binding = </World/Whole>
        def GeomSubset "Left" {{
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </World/Left>
        }}
        def GeomSubset "Right" {{
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [2]
            rel material:binding = </World/Right>
        }}
    }}
    def Camera "Camera" {{
        double3 xformOp:translate = (0,0,10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        float focalLength = 30
    }}
    {materials}
}}
'''


def cyclic_fixture(indirect):
    roots = ("LoopA", "LoopB") if indirect else ("Loop",)
    declarations = ""
    for i, root in enumerate(roots):
        target = roots[(i + 1) % len(roots)]
        declarations += f'''def Xform "{root}" {{
            def PointInstancer "Inner" {{
                rel prototypes = [</World/{target}>]
                int[] protoIndices = [0]
                point3f[] positions = [(0,0,0)]
            }}
        }}'''
    # Put empty cyclic prototypes first: an instance cap cannot bound their
    # recursion because no geometry ever increments the emitted-instance count.
    return fixture("point").replace('def Xform "World" {',
                                    'def Xform "World" {' + declarations, 1)


def cube():
    return '''def Cube "Quad" {
        double size = 2
        rel material:binding = </World/Full>
        rel material:binding:preview = </World/Full>
        rel material:binding:full = </World/Full>
    }'''


def analytic_fixture(kind):
    source = fixture(kind).replace(mesh("Quad"), cube())
    source = source.replace("@prototype.usda@", "@prototype-cube.usda@")
    return source.replace("rel material:binding = </World/Preview>",
                          "rel material:binding = </World/Full>").replace(
        "rel material:binding:preview = </World/Preview>",
        "rel material:binding:preview = </World/Full>")


def nested_root_fixture():
    prototype = f'''def PointInstancer "Inner" {{
        rel prototypes = [</World/Prototypes/Inner/Quad>]
        int[] protoIndices = [0,0]
        point3f[] positions = [(0,-0.5,0),(0,0.5,0)]
        {mesh("Quad")}
    }}'''
    return fixture("point").replace(mesh("Quad"), prototype).replace(
        "rel prototypes = [</World/Prototypes/Quad>]",
        "rel prototypes = [</World/Prototypes/Inner>]")


def inactive_fixture():
    disabled = mesh("Ignored").replace('def Mesh "Ignored" {',
                                        'def Mesh "Ignored" (active = false) {')
    return fixture("point").replace(mesh("Quad"), mesh("Quad") + disabled).replace(
        "rel prototypes = [</World/Prototypes/Quad>]",
        "rel prototypes = [</World/Prototypes>]")


def budget_fixture(kind):
    source = fixture("point")
    if kind == "unresolved":
        return source.replace("rel prototypes = [</World/Prototypes/Quad>]",
                              "rel prototypes = [</Missing>, </World/Prototypes/Quad>]").replace(
            "int[] protoIndices = [0]", "int[] protoIndices = [0,1]").replace(
            "point3f[] positions = [(1.5,0,0)]", "point3f[] positions = [(0,0,0),(1.5,0,0)]")
    source = source.replace("int[] protoIndices = [0]", "int[] protoIndices = [0,0]").replace(
        "point3f[] positions = [(1.5,0,0)]", "point3f[] positions = [(1.5,0,0),(10,0,0)]")
    if kind == "hidden":
        source = source.replace("int[] protoIndices = [0,0]", '''int[] protoIndices = [0,0]
            int64[] ids = [10,20]
            int64[] invisibleIds = [20]''')
    return source


def check_color(path, channels):
    width, height, pixels = read_ppm(path)
    # Both panels must sample the same secondary UV / purpose material. The
    # ordinary panel is also an independent reference for the instanced path.
    for label, center, channel in (("left", 0.28, channels[0]), ("right", 0.72, channels[1])):
        expected = wrong = 0
        for y in range(int(height * 0.4), int(height * 0.6)):
            for x in range(int(width * (center - 0.04)), int(width * (center + 0.04))):
                i = (y * width + x) * 3
                rgb = pixels[i:i + 3]
                if rgb[channel] > 180 and max(rgb[j] for j in range(3) if j != channel) < 80:
                    expected += 1
                else:
                    wrong += 1
        if expected < 100 or wrong > expected * 0.05:
            raise RuntimeError(f"{label} panel: {expected} expected pixels, {wrong} wrong pixels")


def run(binary, output, selected_case=None):
    output.mkdir(parents=True, exist_ok=True)
    (output / "config.json").write_text('{"window_size":{"width":320,"height":320}}')
    # Two solid columns: set 0 selects red, set 1 selects green. Constant UV
    # values make the sampled color deterministic, including with filtering.
    (output / "uv.ppm").write_bytes(b"P6\n2 2\n255\n" + bytes((255, 0, 0, 0, 255, 0)) * 2)
    (output / "prototype.usda").write_text(
        '#usda 1.0\ndef Xform "Proto" {\n' +
        mesh("Quad").replace("/World/", "/Proto/") + materials("/Proto") + '\n}\n')
    (output / "prototype-cube.usda").write_text(
        '#usda 1.0\ndef Xform "Proto" {\n' +
        cube().replace("/World/", "/Proto/") + materials("/Proto") + '\n}\n')
    cases = [(f"{kind}-{purpose}", fixture(kind), purpose, (channel, channel))
             for kind in ("point", "native")
             for purpose, channel in (("preview", 1), ("full", 2))]
    cases.append(("sanitized-subsets", sanitized_fixture(), "preview", (0, 1)))
    cases.extend((f"cycle-{label}", cyclic_fixture(indirect), "preview", (1, 1))
                 for label, indirect in (("self", False), ("indirect", True)))
    cases.extend((f"{kind}-cube", analytic_fixture(kind), "preview", (2, 2))
                 for kind in ("point", "native"))
    cases.append(("nested-root", nested_root_fixture(), "preview", (1, 1)))
    cases.append(("point-inactive", inactive_fixture(), "preview", (1, 1)))
    cases.extend((f"budget-{kind}", budget_fixture(kind), "preview", (1, 1))
                 for kind in ("capped", "hidden", "unresolved"))
    if selected_case:
        cases = [case for case in cases if case[0] == selected_case]
        if not cases:
            raise RuntimeError(f"unknown case: {selected_case}")
    for label, source, purpose, channels in cases:
        asset = output / f"{label}.usda"
        asset.write_text(source)
        capture = output / f"{label}.ppm"
        report = output / f"{label}.json"
        for artifact in (capture, report):
            if artifact.exists():
                artifact.unlink()
        result = subprocess.run([
            str(binary), "--config", str(output / "config.json"),
            "--headless", "--backend", "vk", "--rt", "--next",
            "--size", "320x320", "--no-grid",
            "--camera", "Camera", "--material-purpose", purpose,
            "--mode", "albedo", "--frames", "4", "--screenshot", str(capture),
            "--render-report", str(report),
            str(asset),
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240,
            env={**os.environ, **({"LUSDVIEW_NEXT_MAX_INSTANCES": "1"}
                                  if label.startswith("budget-") else {})})
        (output / f"{label}.log").write_text(result.stdout)
        if "load failed" in result.stdout:
            raise RuntimeError(f"{label}: scene load failed\n{result.stdout}")
        if "caps: v1" not in result.stdout:
            print(f"SKIP: Vulkan backend unavailable\n{result.stdout}")
            return 77
        if not re.search(r"caps: v1 .*rt=hardware", result.stdout):
            print("SKIP: Vulkan hardware ray query unavailable")
            return 77
        if result.returncode or not capture.is_file():
            raise RuntimeError(f"{label}: render failed\n{result.stdout}")
        if label.startswith("native-"):
            count = re.search(r", (\d+) instances,", result.stdout)
            if not count or int(count.group(1)) < 2:
                raise RuntimeError(f"{label}: native instances were flattened or lost\n{result.stdout}")
        if label.startswith("cycle-") and "Skipping cyclic or over-depth instance prototype" not in result.stdout:
            raise RuntimeError(f"{label}: cyclic prototype was not diagnosed\n{result.stdout}")
        summary = json.loads(report.read_text())
        expected_instances = {"point-cube": 1, "native-cube": 2, "nested-root": 4,
                              "point-inactive": 1, "budget-capped": 1,
                              "budget-hidden": 1, "budget-unresolved": 1}
        if label in expected_instances and summary["render"]["total_instances"] != expected_instances[label]:
            raise RuntimeError(f"{label}: expected {expected_instances[label]} instances, "
                               f"got {summary['render']['total_instances']}")
        if label.startswith("budget-") and summary["render"]["truncated"] != (label == "budget-capped"):
            raise RuntimeError(f"{label}: incorrect budget truncation report")
        if label.startswith("cycle-"):
            if summary["load_diagnostics"]["skipped"] < 1 or not any(
                    "instance prototype" in reason for reason in summary["degradation_reasons"]):
                raise RuntimeError(f"{label}: cycle missing from structured diagnostics")
        check_color(capture, channels)
        print(f"PASS: {label} geometry, materials, and diagnostics match")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", help="run a single named generated fixture")
    args = parser.parse_args()
    if args.output:
        raise SystemExit(run(args.binary.resolve(), args.output.resolve(), args.case))
    with tempfile.TemporaryDirectory(prefix="lusdview-prototype-materials-") as directory:
        raise SystemExit(run(args.binary.resolve(), Path(directory), args.case))
