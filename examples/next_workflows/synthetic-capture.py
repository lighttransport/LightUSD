#!/usr/bin/env python3
"""Generate a two-camera mesh dataset; render every product in one process."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def read_pfm(path):
    with path.open('rb') as f:
        assert f.readline() == b'PF\n'
        width, height = map(int, f.readline().split())
        assert float(f.readline()) == -1.0
        data = f.read()
    assert len(data) == width * height * 12
    return width, height, struct.unpack('<' + 'f' * (width * height * 3), data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lusdrender', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    scene = args.output_dir / 'capture.usda'
    scene.write_text('''#usda 1.0
(defaultPrim = "World" upAxis = "Y" metersPerUnit = 1 renderSettingsPrimPath = "/Settings")
def Xform "World" {
  def Mesh "Plane" {
    point3f[] points = [(-10,-10,0), (10,-10,0), (10,10,0), (-10,10,0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0,1,2,3]
    uniform token subdivisionScheme = "none"
    uniform bool doubleSided = true
  }
  def Camera "Left" {
    double3 xformOp:translate = (0,0,5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    float focalLength = 50
    float horizontalAperture = 36
    float verticalAperture = 36
  }
  def Camera "Right" {
    double3 xformOp:translate = (1,0,5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    float focalLength = 50
    float horizontalAperture = 36
    float verticalAperture = 36
  }
}
def RenderVar "Color" { token dataType = "color3f" string sourceName = "color" token sourceType = "raw" }
def RenderVar "Depth" { token dataType = "float" string sourceName = "depth" token sourceType = "raw" }
def RenderVar "Normal" { token dataType = "normal3f" string sourceName = "worldNormal" token sourceType = "raw" }
def RenderVar "Id" { token dataType = "int" string sourceName = "primId" token sourceType = "raw" }
def RenderProduct "Left" {
  token productType = "raster"
  int2 resolution = (16,16)
  rel camera = </World/Left>
  rel orderedVars = [</Color>, </Depth>, </Normal>, </Id>]
}
def RenderProduct "Right" {
  token productType = "raster"
  int2 resolution = (16,16)
  rel camera = </World/Right>
  rel orderedVars = [</Color>, </Depth>, </Normal>, </Id>]
}
def RenderSettings "Settings" { rel products = [</Left>, </Right>] }
''')
    subprocess.run([args.lusdrender, '-rtPreview', '--all-products', str(scene),
                    str(args.output_dir / 'capture.png')], check=True)
    products = []
    for camera in ('Left', 'Right'):
        prefix = args.output_dir / ('capture._2F' + camera)
        depth = Path(str(prefix) + '._2FDepth.pfm')
        normal = Path(str(prefix) + '._2FNormal.pfm')
        ids = Path(str(prefix) + '._2FId.pfm')
        w, h, z = read_pfm(depth)
        _, _, n = read_pfm(normal)
        _, _, labels = read_pfm(ids)
        center = (h // 2 * w + w // 2) * 3
        assert 5 <= z[center] < 5.1, z[center]
        assert abs(n[center + 2] - 1) < 1e-6
        assert labels[center] > 0 and labels[center] == int(labels[center])
        table = json.loads(Path(str(ids) + '.ids.json').read_text())
        assert any(p['id'] == labels[center] and p['primPath'] == '/World/Plane' for p in table['prims'])
        products.append({'camera': '/World/' + camera, 'resolution': [w, h],
                         'translation': [0 if camera == 'Left' else 1, 0, 5],
                         'focalLength': 50, 'aperture': [36, 36],
                         'depth': depth.name, 'normal': normal.name, 'primId': ids.name})
    (args.output_dir / 'manifest.json').write_text(json.dumps({
        'schemaVersion': 1, 'metersPerUnit': 1, 'time': 'default',
        'depthConvention': 'camera ray distance in stage units; background 0',
        'normalConvention': 'world space, signed xyz; background (0,0,0)',
        'sampling': 'pixel center, one sample for data AOVs',
        'products': products}, indent=2) + '\n')


if __name__ == '__main__':
    main()
