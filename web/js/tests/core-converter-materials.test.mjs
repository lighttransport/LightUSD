import assert from 'node:assert/strict';
import fs from 'node:fs';
import {loadWasm, unpackUSDZ, convertFolderToUSDZ, convertSourceToUSDZStreaming} from '../src/usdzconvert.js';

const suffix = process.env.LIGHTUSD_WASM64 === '1' ? '_64' : '';
const native = await loadWasm(
  () => import(`../src/lightusd/lightusd${suffix}.js`),
  {locateFile: file => new URL('../src/lightusd/' + file, import.meta.url).pathname});
const source = fs.readFileSync(new URL(
  '../../../tests/legacy/unit/fixtures/usdzconvert-regression-textured-two-materials.usda',
  import.meta.url));
const converter = new native.NextUSDZConverterNative();
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
    const reader = new native.NextUSDZConverterNative();
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
    const log=[];
    const input=new Map([['scene.usda',semantic]]);
    const options={rootLayerFormat,optimizeMaterials,reencode:false,log:line=>log.push(line)};
    const result = streaming ? await convertSourceToUSDZStreaming(native,{keys:[...input.keys()],fetch:async key=>input.get(key)},options)
      :await convertFolderToUSDZ(native,input,options);
    if(optimizeMaterials==='atlas')assert.ok(log.some(line=>/atlas generation is unavailable/i.test(line)),log.join('\n'));
    const reader = new native.NextUSDZConverterNative();
    try {
      assert.ok(reader.loadFromBinary(result.usdz,'preview.usdz'),reader.error());
      const text = reader.exportAsUSDA();
      assert.equal((text.match(/def Material /g)||[]).length,1,text);
      assert.match(text,/material:binding = <\/Root\/A>/);
    } finally {reader.delete();}
  }
  console.log('PASS: core converter exact/PreviewSurface material dedupe and remapped USDA/USDC exports');
} finally { converter.delete(); }
