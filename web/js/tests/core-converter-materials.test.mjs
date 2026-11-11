import assert from 'node:assert/strict';
import fs from 'node:fs';
import {PNG} from 'pngjs';
import {loadWasm, unpackUSDZ, convertFolderToUSDZ, convertSourceToUSDZStreaming} from '../src/usdzconvert.js';

const suffix = process.env.LIGHTUSD_WASM64 === '1' ? '_64' : '';
const native = await loadWasm(
  () => import(`../src/lightusd/lightusd${suffix}.js`),
  {locateFile: file => new URL('../src/lightusd/' + file, import.meta.url).pathname});
const source = fs.readFileSync(new URL(
  '../../../tests/legacy/unit/fixtures/usdzconvert-regression-textured-two-materials.usda',
  import.meta.url));
const converter = new native.USDZConverterNative();
try {
  assert.equal(converter.loadFromBinary(new Uint8Array(source), 'scene.usda'), true,
    converter.error());
  converter.setAsset('Texture.png', new Uint8Array([1, 2, 3]));
  for (const rootLayerFormat of ['usda', 'usdc']) {
    const bytes = converter.exportAsUSDZWithOptions(
      {'Texture.png': 'textures/renamed.png'},
      {rootLayerFormat, optimizeMaterials: 'dedupe'});
    assert.ok(bytes?.length, converter.error());
    const archive = unpackUSDZ(bytes);
    assert.ok(archive.entries.has('textures/renamed.png'));
    const reader = new native.USDZConverterNative();
    try {
      assert.equal(reader.loadFromBinary(bytes, 'converted.usdz'), true, reader.error());
      const text = reader.exportAsUSDA();
      assert.equal((text.match(/def Material /g) || []).length, 1, text);
      assert.ok(!text.includes('/Root/MatB'), text);
      assert.ok(text.includes('@textures/renamed.png@'), text);
    } finally { reader.delete(); }
  }
  assert.ok(converter.exportAsUSDA().includes('def Material "MatB"'),
    'optimized export must not mutate the source stage');
  assert.equal(converter.exportAsUSDZWithOptions({optimizeGeometry: 'decimate'}), null);
  assert.match(converter.error(), /Unsupported/);
  const semantic = new TextEncoder().encode(`#usda 1.0
 def Xform "Root" {
  def Material "A" {
   token outputs:surface.connect=</Root/A/Surface.outputs:surface>
   def Shader "Surface" { uniform token info:id="UsdPreviewSurface"; float inputs:roughness=0.5; }
  }
  def Material "B" {
   token outputs:surface.connect=</Root/B/Renamed.outputs:surface>
   def Shader "Renamed" { uniform token info:id="UsdPreviewSurface"; float inputs:roughness=0.5; }
   def Shader "Unused" { uniform token info:id="UnknownShader"; }
  }
  def Scope "Bound" { rel material:binding=</Root/B>; }
 }
 `);
  for (const rootLayerFormat of ['usda','usdc'])for(const optimizeMaterials of ['preview','atlas'])for(const streaming of [false,true]) {
    const input=new Map([['scene.usda',semantic]]);
    const options={rootLayerFormat,optimizeMaterials,reencode:false};
    const result = streaming ? await convertSourceToUSDZStreaming(native,{keys:[...input.keys()],fetch:async key=>input.get(key)},options)
      :await convertFolderToUSDZ(native,input,options);
    const reader = new native.USDZConverterNative();
    try {
      assert.ok(reader.loadFromBinary(result.usdz,'preview.usdz'),reader.error());
      const text = reader.exportAsUSDA();
      assert.equal((text.match(/def Material /g)||[]).length,1,text);
      assert.match(text,/material:binding = <\/Root\/A>/);
    } finally {reader.delete();}
  }
  console.log('PASS: core converter exact/PreviewSurface material dedupe and remapped USDA/USDC exports');
} finally { converter.delete(); }

const colors = {Green:[0,255,0,128], Red:[255,0,0,255]};
const graph = (name) => `
 def Material "${name}" {
  token outputs:surface.connect = </Root/${name}/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor.connect = </Root/${name}/Texture.outputs:rgb>
   float inputs:opacity.connect = </Root/${name}/Texture.outputs:a>
   token outputs:surface
  }
  def Shader "Texture" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @${name}.png@
   token inputs:sourceColorSpace = "sRGB"
   token inputs:wrapS = "clamp"
   token inputs:wrapT = "clamp"
   float2 inputs:st.connect = </Root/${name}/UV.outputs:result>
   float3 outputs:rgb
   float outputs:a
  }
  def Shader "UV" {
   uniform token info:id = "UsdPrimvarReader_float2"
   token inputs:varname = "st"
   float2 outputs:result
  }
 }
 def Mesh "Mesh${name}" {
  uniform token subdivisionScheme = "none"
  point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  texCoord2f[] primvars:st = [(0,0), (1,0), (0,1)] (interpolation = "faceVarying")
  rel material:binding = </Root/${name}>
 }
`;
const rootPath = 'scene/root.usda';
const root = new TextEncoder().encode('#usda 1.0\ndef Xform "Root" {\n' +
  Object.keys(colors).map(graph).join('\n') + '}\n');
const input = new Map([[rootPath,root]]);
for (const [name, color] of Object.entries(colors)) {
  const png = new PNG({width:2,height:2});
  for (let i=0; i<png.data.length; i+=4) png.data.set(color,i);
  input.set('scene/'+name+'.png',PNG.sync.write(png));
}
for (const streaming of [false,true]) for (const rootLayerFormat of ['usda','usdc']) {
  const options = {rootPath, rootLayerFormat, optimizeMaterials:'atlas',
    materialAtlasSize:16, materialAtlasTileSize:2, materialAtlasPadding:1,
    textureFormat:'jpeg', reencode:true};
  const result = streaming ? await convertSourceToUSDZStreaming(native,{
    keys:[...input.keys()],fetch:async key=>input.get(key),
  },options) : await convertFolderToUSDZ(native,input,options);
  const archive = unpackUSDZ(result.usdz);
  const atlases = [...archive.entries].filter(([name])=>name.includes('/material_atlas_'));
  assert.equal(atlases.length,1,archive.order.join(', '));
  const [[atlasPath, bytes]] = atlases;
  const png = PNG.sync.read(Buffer.from(bytes));
  assert.deepEqual([png.width,png.height],[8,8]);
  const rgba = (x,y) => [...png.data.subarray(4*(y*png.width+x),4*(y*png.width+x+1))];
  assert.deepEqual(rgba(1,6),colors.Green);
  assert.deepEqual(rgba(5,6),colors.Red);
  assert.deepEqual(rgba(0,7),colors.Green);
  assert.deepEqual(rgba(4,7),colors.Red);
  const reader = new native.USDZConverterNative();
  try {
    assert.ok(reader.loadFromBinary(result.usdz,'atlas.usdz'),reader.error());
    const text = reader.exportAsUSDA();
    assert.equal((text.match(/def Material /g)||[]).length,1,text);
    assert.match(text,/primvars:lightusdMaterialAtlasUV/);
    assert.ok(text.includes('@'+atlasPath+'@'),text);
  } finally {reader.delete();}
}
console.log('PASS: material atlas pixels, UVs and packaged assets in folder and streaming conversion');
