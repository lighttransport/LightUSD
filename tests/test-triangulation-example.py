#!/usr/bin/env python3
"""Check original polygon statistics and both core triangulation policies."""
import pathlib
import subprocess
import sys
import tempfile
app = sys.argv[1]
with tempfile.TemporaryDirectory(prefix='lightusd-triangulation-') as directory:
    root = pathlib.Path(directory)
    source = root/'polygons.usda'
    source.write_text('''#usda 1.0
def Mesh "Mixed" {
 uniform token subdivisionScheme="none"
 point3f[] points=[(0,0,0),(1,0,0),(1,1,0),(0,1,0),(-0.5,0.5,0)]
 int[] faceVertexCounts=[3,4,5]
 int[] faceVertexIndices=[0,1,2,0,1,2,3,0,1,2,3,4]
}
''')
    for method in [None, 'earcut', 'fan']:
        result = subprocess.run([app,str(source)]+([method] if method else []),capture_output=True,text=True)
        assert result.returncode == 0, result.stdout+result.stderr
        for expected in ['Number of meshes: 1','Total original faces: 3','Total triangles: 6',
                         '3-gons: 1','4-gons: 1','5-gons: 1']:
            assert expected in result.stdout, result.stdout
    dependency = root/'reference.usda'
    dependency.write_text('#usda 1.0\ndef Mesh "Ref" (references=@polygons.usda@</Mixed>) {}\n')
    result = subprocess.run([app,str(dependency),'earcut'],capture_output=True,text=True)
    assert result.returncode == 0 and 'Total triangles: 6' in result.stdout, result.stdout+result.stderr
    for args in [[], [str(source),'wrong'],[str(root/'missing.usda')],[str(source),'fan','extra']]:
        result = subprocess.run([app,*args],capture_output=True,text=True)
        assert result.returncode != 0
print('PASS: core triangulation example, original polygon counts, both methods, references and CLI errors')
