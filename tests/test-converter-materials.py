#!/usr/bin/env python3
"""Exact material dedupe preserves distinct shaders and retargets bindings."""
import pathlib
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
        if mode == 'atlas':
            assert 'atlas generation is unavailable' in result.stderr.lower(), result.stderr
        text = output.read_text()
        assert text.count('def Material ') == count, (mode, text)
        assert all(f'def Material \"{name}\"' in text for name in ['C', 'D', 'E', 'F']), text
        if mode in ('preview', 'atlas'):
            assert 'def Material "B"' not in text, text
            assert 'rel material:binding = </Root/A>' in text, text
