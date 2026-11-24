#!/usr/bin/env python3
"""Exact material dedupe preserves distinct shaders and retargets bindings."""
import pathlib
import re
import struct
import zipfile
import zlib
import subprocess
import sys
import tempfile

converter = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="lightusd-materials-") as directory:
    directory = pathlib.Path(directory)
    scene = directory / "scene.usda"
    source = '#usda 1.0\ndef Xform "Root" {\n'
    for name, roughness in [("A", "0.5"), ("B", "0.5"), ("C", "0.50000006")]:
        source += f'''def Material "{name}" {{
 token outputs:surface.connect = </Root/{name}/Surface.outputs:surface>
 def Shader "Surface" {{
  uniform token info:id = "UsdPreviewSurface"
  float inputs:roughness = {roughness}
  token outputs:surface
 }}
}}
'''
    source += '''def Mesh "Mesh" {
 point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 rel material:binding = </Root/B>
}
}
'''
    scene.write_text(source)
    output = directory / "output.usda"
    subprocess.run([converter, str(scene), str(output), "--outputFormat", "usda",
                    "-optimizeMaterials", "dedupe"], check=True)
    text = output.read_text()
    assert text.count("def Material ") == 2, text
    assert 'def Material "B"' not in text, text
    assert '</Root/B' not in text, text
    assert 'rel material:binding = </Root/A>' in text, text
    assert 'def Material "C"' in text, text
    # Preview comparison canonicalizes the surface name and ignores unused
    # shaders, while preserving distinct values and unsupported output graphs.
    materials = []
    for name, shader, value in [('A', 'Surface', '0.5'), ('B', 'Renamed', '0.5'),
                                ('C', 'Surface', '0.50000006'), ('D', 'Surface', '0.5')]:
        extra = ' token outputs:displacement.connect = </Root/D/Surface.outputs:displacement>\n' if name == 'D' else ''
        unused = ' def Shader "Unused" { uniform token info:id = "UnknownShader"; }\n' if name == 'B' else ''
        materials.append(f'''def Material "{name}" {{
 token outputs:surface.connect = </Root/{name}/{shader}.outputs:surface>
{extra} def Shader "{shader}" {{
  uniform token info:id = "UsdPreviewSurface"
  float inputs:roughness = {value}
  token outputs:surface
 }}
{unused}}}
''')
    for name, shader in [('E', 'Surface'), ('F', 'Renamed')]:
        materials.append(f'''def Material "{name}" {{
 token outputs:surface.connect = </Root/{name}/{shader}.outputs:surface>
 def Shader "{shader}" {{
  uniform token info:id = "UsdPreviewSurface"
  float inputs:roughness.connect = </Root/{name}/Tex.outputs:r>
 }}
 def Shader "Tex" {{
  uniform token info:id = "UsdUVTexture"
  float2 inputs:st.connect = </Root/{name}/{shader}.outputs:surface>
 }}
}}
''')
    scene.write_text('#usda 1.0\ndef Xform "Root" {\n' + ''.join(materials) +
                     'def Scope "Bound" { rel material:binding = </Root/B>; }\n}\n')
    for mode, count in [('dedupe', 6), ('preview', 5), ('atlas', 5)]:
        result = subprocess.run([converter, str(scene), str(output), '--outputFormat', 'usda',
                                '-optimizeMaterials', mode], text=True, capture_output=True)
        assert result.returncode == 0, result.stderr
        text = output.read_text()
        assert text.count('def Material ') == count, (mode, text)
        assert all(f'def Material \"{name}\"' in text for name in ['C', 'D', 'E', 'F']), text
        if mode in ('preview', 'atlas'):
            assert 'def Material "B"' not in text, text
            assert 'rel material:binding = </Root/A>' in text, text

    def png_chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))

    def solid_png(rgb):
        return (b"\x89PNG\r\n\x1a\n" +
                png_chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)) +
                png_chunk(b"IDAT", zlib.compress((b"\0" + bytes(rgb) * 2) * 2)) +
                png_chunk(b"IEND", b""))

    (directory / "red.png").write_bytes(solid_png((255, 0, 0)))
    (directory / "green.png").write_bytes(solid_png((0, 255, 0)))
    graph = """
def Material "{name}" {{
 token outputs:surface.connect = </Root/{name}/Surface.outputs:surface>
 def Shader "Surface" {{
  uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor.connect = </Root/{name}/Tex.outputs:rgb>
  token outputs:surface
 }}
 def Shader "Tex" {{
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @{image}.png@
  token inputs:sourceColorSpace = "sRGB"
  token inputs:wrapS = "{wrap}"
  token inputs:wrapT = "clamp"
  float2 inputs:st.connect = </Root/{name}/UV.outputs:result>
  float3 outputs:rgb
 }}
 def Shader "UV" {{
  uniform token info:id = "UsdPrimvarReader_float2"
  token inputs:varname = "st"
  float2 outputs:result
 }}
}}
def Mesh "Mesh{name}" {{
 uniform token subdivisionScheme = "none"
 point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 texCoord2f[] primvars:st = [(0,0), (1,0), (0,1)] (interpolation = "faceVarying")
 int[] primvars:st:indices = [2,0,1]
 rel material:binding = </Root/{name}>
}}
"""
    scene.write_text('#usda 1.0\ndef Xform "Root" {\n' +
                     graph.format(name="Red", image="red", wrap="clamp") +
                     graph.format(name="Green", image="green", wrap="clamp") +
                     graph.format(name="Repeat", image="red", wrap="repeat") + "}\n")
    package = directory / "atlas.usdz"
    args = ["-optimizeMaterials", "atlas", "-materialAtlasSize", "16",
            "-materialAtlasTileSize", "2", "-materialAtlasPadding", "1"]
    subprocess.run([converter, str(scene), str(package), "--rootLayerFormat", "usda",
                    *args], check=True)
    with zipfile.ZipFile(package) as archive:
        atlas, = [name for name in archive.namelist() if "material_atlas_" in name]
        root = archive.read(archive.namelist()[0]).decode()
        assert root.count("def Material ") == 2, root
        assert root.count(f"@{atlas}@") == 1, root
        assert root.count("rel material:binding = </Root/Green>") == 2, root
        assert "rel material:binding = </Root/Repeat>" in root, root
        assert root.count("primvars:lightusdMaterialAtlasUV:indices = [2, 0, 1]") == 2, root
        assert "red.png" in archive.namelist(), archive.namelist()
        assert "green.png" not in archive.namelist(), archive.namelist()
        assert struct.unpack_from(">II", archive.read(atlas), 16) == (8, 8)
        uv_lists = re.findall(r"primvars:lightusdMaterialAtlasUV = \[(.*?)\]", root)
        assert len(uv_lists) == 2 and len(set(uv_lists)) == 2, root
        assert "(0.125, 0.125)" in root and "(0.625, 0.125)" in root, root

    subprocess.run([converter, str(scene), str(output), "--outputFormat", "usda", *args], check=True)
    match = re.search(r"@(textures/material_atlas_[^@]+)@", output.read_text())
    assert match and (directory / match.group(1)).is_file(), output.read_text()


    # Quality fitting must account for the atlas without transcoding it.
    subprocess.run([converter, str(scene), str(package), *args,
                    "-targetTextureSize", "1MB", "-fitStrategy", "quality"], check=True)
    with zipfile.ZipFile(package) as archive:
        atlas, = [name for name in archive.namelist() if "material_atlas_" in name]
        assert archive.read(atlas).startswith(b"\x89PNG\r\n\x1a\n")

    # Repacking a stored USDZ must resolve the image relative to its inner root.
    source_package = directory / "source.usdz"
    with zipfile.ZipFile(source_package, "w") as archive:
        archive.writestr("nested/scene.usda",
                         '#usda 1.0\ndef Xform "Root" {\n' +
                         graph.format(name="Red", image="red", wrap="clamp") +
                         graph.format(name="Green", image="green", wrap="clamp") + "}\n")
        for image in ("red.png", "green.png"):
            archive.write(directory / image, "nested/" + image)
    subprocess.run([converter, str(source_package), str(package), *args], check=True)
    with zipfile.ZipFile(package) as archive:
        assert len(archive.namelist()) == 2, archive.namelist()
        assert "material_atlas_" in archive.namelist()[1], archive.namelist()
