#!/usr/bin/env python3
"""Exercise the public core scene-query example through composed references."""
import pathlib
import subprocess
import sys
import tempfile
app = sys.argv[1]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    (root / 'asset.usda').write_text('''#usda 1.0
(defaultPrim="World")
def Xform "World" {
 rel material:binding=</World/Look>
 def Mesh "Mesh" {}
 def Material "Look" {
  token outputs:surface.connect=</World/Look/Surface.outputs:surface>
  def Shader "Reader" {
   uniform token info:id="UsdPrimvarReader_float2"
   token inputs:varname="st"
   float2 outputs:result
  }
  def Shader "Surface" {
   uniform token info:id="UsdPreviewSurface"
   float inputs:roughness=0.25
   token outputs:surface
  }
 }
}
''')
    (root / 'root.usda').write_text('''#usda 1.0
def Xform "Scene" (references=@asset.usda@) {}
''')
    result = subprocess.run([app, str(root / 'root.usda')], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    for expected in ['Meshes: 1, Xforms: 1, Materials: 1, Shaders: 2',
                     'directlyBoundMaterial: \n', 'boundMaterial: /Scene/Look',
                     'parent: /Scene', 'surface: /Scene/Look/Surface',
                     'id: UsdPreviewSurface', 'evaluated inputs:varname = "st"', '0.25']:
        assert expected in result.stdout, (expected, result.stdout)
    result = subprocess.run([app, str(root / 'missing.usda')], capture_output=True)
    assert result.returncode != 0
print('PASS: core Tydra API example references, hierarchy, inherited binding and shader evaluation')
