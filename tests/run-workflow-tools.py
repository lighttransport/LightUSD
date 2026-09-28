#!/usr/bin/env python3
"""Hermetic workflow integration tests. All generated assets stay in a temp dir."""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", required=True)
    parser.add_argument("--examples-only", action="store_true")
    parser.add_argument("--capture-renderer")
    args = parser.parse_args()
    binaries = Path(args.bin_dir).resolve()

    def run(tool, *argv, code=0):
        path = binaries / tool
        if tool == "lusddumpcrate":
            path = binaries / "tools/lusddumpcrate/lusddumpcrate"
        result = subprocess.run([str(path), *map(str, argv)], text=True, capture_output=True)
        assert result.returncode == code, (tool, argv, result.returncode, result.stdout, result.stderr)
        return result.stdout

    with tempfile.TemporaryDirectory(prefix="lightusd-workflows-") as temp:
        work = Path(temp)
        scene = work / "root.usda"
        scene.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y" metersPerUnit = 1)
def Xform "World" (variants = { string display = "low" } prepend variantSets = "display") {
  variantSet "display" = {
    "low" { custom int level = 1 }
    "high" { custom int level = 2 }
  }
  double radius = 4
  double radius.timeSamples = {0: 1, 1: 3}
  double splineValue = 0
  double splineValue.spline = {0: 0; post linear, 1: 2,}
  def SkelAnimation "Motion" {
    uniform token[] joints = ["root"]
    float3[] translations = [(0,0,0)]
    quatf[] rotations = [(1,0,0,0)]
    half3[] scales = [(1,1,1)]
    float3[] translations.timeSamples = {0: [(0,0,0)], 1: [(1,0,0)]}
  }
  def Mesh "Triangle" {
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    uniform token subdivisionScheme = "none"
  }
  def Xform "Detail" (prepend payload = @detail.usda@</Detail>) {}
}
''')
        (work / "detail.usda").write_text('#usda 1.0\ndef Xform "Detail" {}\n')
        crate = work / "scene.usdc"
        run("next_quickstart", scene, crate)
        assert crate.read_bytes().startswith(b"PXR-USDC")
        out = run("animation_sampling", scene, "/World.radius")
        assert "linear 0.500000 = 2" in out and "held 0.500000 = 1" in out
        assert "default = 4" in out
        assert "skeleton sample /World/Motion joints=1" in out
        spline = run("animation_sampling", scene, "/World.splineValue")
        assert "linear 0.500000 = 1" in spline
        run("next_quickstart", scene, work / "roundtrip.usda")
        assert (work / "roundtrip.usda").read_text().startswith("#usda")
        interactive = run("interactive_session", scene)
        assert "changes 0 -> 1 prims=1 properties=0" in interactive, interactive
        assert "property /World.level" in interactive
        assert "transferred stage without cloning" in interactive
        assert "released composition cache; retained snapshot remains readable" in interactive
        assert "dependency " in interactive
        assert "read-only snapshot stage queried without cloning" in interactive
        assert "provisional preview phase=0" in interactive
        assert "provisional preview phase=1" in interactive
        assert "released static geometry properties=" in interactive
        run("portable_asset_package", work / "package")
        if (binaries / "usd_to_gltf").exists():
            glb = work / "scene.glb"
            run("usd_to_gltf", scene, glb)
            run("usd_to_gltf", scene, work / "strict.glb", "--strict", code=1)
            assert not (work / "strict.glb").exists()
            data = glb.read_bytes()
            magic, version, length, json_size, chunk = struct.unpack_from("<5I", data)
            assert magic == 0x46546c67 and version == 2 and length == len(data)
            assert chunk == 0x4e4f534a
            doc = json.loads(data[20:20 + json_size])
            assert doc["meshes"] and doc["nodes"]
            pos = doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]
            assert doc["accessors"][pos]["count"] == 3
            assert any("animation" in x for x in doc["extras"]["lightusdConversionLosses"])
        if args.examples_only:
            return
        textured = work / "textured.usda"
        (work / "pixel.png").write_bytes(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aOGkAAAAASUVORK5CYII="))
        textured.write_text('''#usda 1.0
(metersPerUnit = 1 upAxis = "Y")
def Mesh "Mesh" {
 point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 texCoord2f[] primvars:st = [(0,0),(1,0),(0,1)] (interpolation = "faceVarying")
 uniform token subdivisionScheme = "none"
 uniform bool doubleSided = true
 rel material:binding = </Mat>
}
def Material "Mat" {
 token outputs:surface.connect = </Mat/Surface.outputs:surface>
 def Shader "Surface" {
  uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor.connect = </Mat/Tex.outputs:rgb>
  token outputs:surface
 }
 def Shader "Tex" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @pixel.png@
  float3 outputs:rgb
 }
}
''')
        textured_glb = work / "textured.glb"
        run("usd_to_gltf", textured, textured_glb)
        content = textured_glb.read_bytes()
        size = struct.unpack_from("<I", content, 12)[0]
        textured_doc = json.loads(content[20:20 + size])
        assert "images" in textured_doc, textured_doc
        assert textured_doc["images"][0]["mimeType"] == "image/png"
        prim = textured_doc["meshes"][0]["primitives"][0]
        assert "TEXCOORD_0" in prim["attributes"]
        mat = textured_doc["materials"][prim["material"]]
        assert mat["doubleSided"] and "baseColorTexture" in mat["pbrMetallicRoughness"]
        textured_package = work / "textured.usdz"
        with zipfile.ZipFile(textured_package, "w", zipfile.ZIP_STORED) as package:
            package.writestr("root.usda", textured.read_text())
            package.writestr("pixel.png", (work / "pixel.png").read_bytes())
        run("usd_to_gltf", textured_package, work / "packaged.glb")
        packaged = (work / "packaged.glb").read_bytes()
        packaged_size = struct.unpack_from("<I", packaged, 12)[0]
        assert json.loads(packaged[20:20 + packaged_size])["images"]
        packaged_deps = json.loads(run(
            "lusddeps", "--composed", "--json", textured_package))
        assert packaged_deps["complete"]
        assert any(d["resolvedIdentifier"].endswith(
            "textured.usdz[pixel.png]") for d in packaged_deps["dependencies"])
        bad_package = work / "missing.usdz"
        with zipfile.ZipFile(bad_package, "w", zipfile.ZIP_STORED) as package:
            package.writestr("root.usda", textured.read_text())
        missing = json.loads(run("lusddeps", "--json", bad_package, code=1))
        assert any(d["authoredPath"] == "pixel.png" and d["status"] == "missing" for d in missing["dependencies"])
        dump = json.loads(run("lusddumpcrate", "--format", "json", crate))
        assert dump["bootstrap"]["magic"] == "PXR-USDC"
        assert dump["tokens"]["count"] > 0 and dump["specs"]["values"]
        limited = json.loads(run("lusddumpcrate", "--format", "json", "--limit-tokens", "1", crate))
        assert limited["tokens"]["truncated"] and len(limited["tokens"]["values"]) == 1
        report = json.loads(run("lusddeps", "--json", scene))
        assert report["complete"]
        assert any(d["kind"] == "payload" for d in report["dependencies"])
        limited = json.loads(run("lusddeps", "--json", "--max-layers", "1", scene, code=1))
        assert not limited["complete"] and limited["diagnostics"]
        variants = work / "variants.usda"
        variants.write_text('''#usda 1.0
def Xform "Asset" (variants = { string lod = "low" } prepend variantSets = "lod") {
 variantSet "lod" = {
  "low" { custom asset texture = @pixel.png@ }
  "high" { custom asset texture = @missing.png@ }
 }
}
''')
        assert json.loads(run("lusddeps", "--json", variants))["complete"]
        all_variants = json.loads(run("lusddeps", "--json", "--all-variants", variants, code=1))
        assert any(d["status"] == "missing" and "high" in d["location"] for d in all_variants["dependencies"])
        weak = work / "weak.usda"
        weak.write_text('''#usda 1.0
(expressionVariables = { string TEX = "missing.png" })
def Xform "Asset" (variants = { string lod = "high" } prepend variantSets = "lod") {
 variantSet "lod" = {
  "low" { custom asset texture = @`${TEX}`@ }
  "high" { custom asset texture = @missing.png@ }
 }
}
''')
        strong = work / "strong.usda"
        strong.write_text('''#usda 1.0
(subLayers = [@weak.usda@]
 expressionVariables = { string TEX = "pixel.png" })
over "Asset" (variants = { string lod = "low" }) {}
''')
        composed = json.loads(run("lusddeps", "--json", "--composed", strong))
        assert composed["scope"] == "composedStage" and composed["complete"]
        assert any(d["authoredPath"] == "`${TEX}`" and
                   d["resolvedIdentifier"].endswith("pixel.png")
                   for d in composed["dependencies"])
        assert not any(d["authoredPath"] == "missing.png" for d in composed["dependencies"])
        run("lusddeps", "--max-records", "0", scene, code=2)
        variant_trace = json.loads(run("lusdcat", "--explain", "/Asset.texture", "--json", variants))
        assert any("{lod=low}" in o["primPath"] and o["layer"].endswith("variants.usda") for o in variant_trace["opinions"])
        trace = json.loads(run("lusdcat", "--explain", "/World.radius", "--time", "0.5", "--json", scene))
        assert trace["resolution"]["value"] == "2"
        assert trace["resolution"]["source"] == "timeSample"
        assert trace["opinions"][0]["hasSamples"]
        assert trace["opinions"][0]["sampleTimes"] == [0, 1]
        connected = work / "connected.usda"
        connected.write_text('''#usda 1.0
def Xform "Graph" {
  float inputs:a.connect = </Graph.inputs:b>
  float inputs:b.connect = </Graph.inputs:c>
  float inputs:c = 7
}
''')
        connection_trace = json.loads(run(
            "lusdcat", "--explain", "/Graph.inputs:a", "--json", connected))
        assert connection_trace["resolution"]["value"] == "7"
        assert connection_trace["connectionChain"] == [
            "/Graph.inputs:b", "/Graph.inputs:c"]
        assert connection_trace["opinions"][0]["connections"] == [
            "/Graph.inputs:b"]
        invalid = work / "invalid.usda"
        invalid.write_text('#usda 1.0\n(metersPerUnit = -1)\ndef Xform "World" {}\n')
        baseline = work / "baseline.json"
        baseline.write_text(run("lusdchecker", "--core-only", "--json", invalid, code=1))
        report = json.loads(run("lusdchecker", "--core-only", "--json", "--baseline", baseline, invalid))
        assert not report["valid"] and report["gatePassed"]
        assert report["existingIssueCount"] > 0 and report["newIssueCount"] == 0
        sarif = json.loads(run("lusdchecker", "--core-only", "--sarif", invalid, code=1))
        assert sarif["version"] == "2.1.0" and sarif["runs"][0]["results"]
        malformed = work / "bad.json"
        malformed.write_text('{}')
        run("lusdchecker", "--baseline", malformed, invalid, code=2)
        left, right = work / "left", work / "right"
        left.mkdir(); right.mkdir()
        (left / "a.usda").write_text('#usda 1.0\ndef Xform "A" { int x = 1 }\n')
        (right / "a.usda").write_text('#usda 1.0\n\ndef Xform "A" { int x = 1 }\n')
        run("lusddiff", left, right)
        (right / "image.png").write_bytes(b"asset")
        report = json.loads(run("lusddiff", "--json", left, right, code=1))
        assert report["entries"][0]["status"] == "added"
        for name in ("a", "b"):
            with zipfile.ZipFile(work / (name + ".usdz"), "w", zipfile.ZIP_STORED) as package:
                package.writestr("root.usda", '#usda 1.0\ndef Xform "A" {}\n')
                package.writestr("texture.png", name.encode())
        report = json.loads(run("lusddiff", "--recursive", "--json", work / "a.usdz", work / "b.usdz", code=1))
        assert len(report["entries"]) == 1 and report["entries"][0]["comparison"] == "bytes"
        if args.capture_renderer:
            subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / "examples/next_workflows/synthetic-capture.py"),
                            "--lusdrender", str(Path(args.capture_renderer).resolve()),
                            "--output-dir", str(work / "capture")], check=True)
    print("Workflow tests passed")


if __name__ == "__main__":
    main()
