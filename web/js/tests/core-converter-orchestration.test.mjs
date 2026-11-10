// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {loadWasm, convertFolderToUSDZ, convertSourceToUSDZStreaming, unpackUSDZ, buildUSDZWithNewRoot} from '../src/usdzconvert.js';
const suffix = process.env.LIGHTUSD_WASM64 === '1' ? '_64' : '';
const native = await loadWasm(() => import(`../src/lightusd/lightusd${suffix}.js`), {
  locateFile: file => new URL('../src/lightusd/' + file, import.meta.url).pathname,
});
const encode = text => new TextEncoder().encode(text);
function textFromPackage(bytes) {
  const reader = new native.NextUSDZConverterNative();
  try {
    assert.equal(reader.loadFromBinary(bytes, 'result.usdz'), true, reader.error());
    return reader.exportAsUSDA();
  } finally { reader.delete(); }
}
const root = `#usda 1.0
(defaultPrim="Root"; subLayers=[@../weak/look.usda@])
def Xform "Root" (prepend variantSets="look"; variants={string look="red"}) {
  variantSet "look"={"red" {float gain=1;} "blue" {float gain=2;}}
  over "Shared" {float inputs:strength=7;}
  def Xform "Red" (prepend references=@../red/model.usda@</Asset>) {}
  def Xform "Blue" (prepend references=@../blue/model.usda@</Asset>) {}
}
`;
const dependency = `#usda 1.0
def Xform "Asset" {
  def Shader "Texture" {
    uniform token info:id="UsdUVTexture"
    asset inputs:file=@tex.png@
    asset inputs:animated.timeSamples={0:@tex.png@,1:@alt.png@}
  }
}`;
const input = new Map([
  ['scene/root.usda', encode(root)],
  ['weak/look.usda', encode('#usda 1.0\ndef Xform "Root" {def Shader "Shared" {asset inputs:file=@tex.png@;}}')],
  ['red/model.usda', encode(dependency)], ['blue/model.usda', encode(dependency)],
  ['weak/tex.png', new Uint8Array([1])],
  ['red/tex.png', new Uint8Array([2])], ['red/alt.png', new Uint8Array([3])],
  ['blue/tex.png', new Uint8Array([4])], ['blue/alt.png', new Uint8Array([5])],
]);
// Retained-layer export preserves directories, dependency bytes and anchors.
for(const streaming of [false,true])for(const rootLayerFormat of ['usda','usdc']) {
 const options={rootPath:'scene/root.usda',rootLayerFormat,flatten:false,reencode:false};
 const result=streaming?await convertSourceToUSDZStreaming(native,{keys:[...input.keys()],fetch:async key=>input.get(key)},options)
  :await convertFolderToUSDZ(native,input,options);
 assert.equal(result.stats.flatten,false);assert.ok(result.stats.layersPackaged>=3);
 const archive=unpackUSDZ(result.usdz);
 assert.equal(archive.order[0],'scene/root.'+rootLayerFormat);
 for(const [key,bytes] of input)if(key!==archive.order[0])assert.deepEqual(archive.entries.get(key),bytes,key);
 const text=textFromPackage(result.usdz);
 assert.match(text,/references = @\.\.\/red\/model.usda@/);
 assert.match(text,/variantSet "look"/);assert.ok(!text.includes('inputs:animated.timeSamples'));
 const flattened=await convertFolderToUSDZ(native,new Map([['packed.usdz',result.usdz]]),{reencode:false});
 const flatText=textFromPackage(flattened.usdz);
 assert.match(flatText,/@red\/tex.png@/);assert.match(flatText,/@blue\/tex.png@/);
 assert.ok(!flatText.includes('references ='));
}
// Format changes must not overwrite a dependency with the desired root name.
const colliding=new Map(input);colliding.set('scene/root.usdc',encode('#usda 1.0\ndef Scope "Dependency" {}\n'));
colliding.set('unreferenced.exr',new Uint8Array([1,2,3]));
const rawNative={...native,loadFeature:()=>{throw Error('Retained bytes must not load image codecs');}};
const retainedChunks=[];
const retained=await convertSourceToUSDZStreaming(rawNative,{keys:[...colliding.keys()],fetch:async key=>colliding.get(key)},
 {rootPath:'scene/root.usda',flatten:false,rootLayerFormat:'usdc',zipSink:bytes=>retainedChunks.push(bytes.slice())});
assert.equal(retained.usdz,null);assert.equal(retained.stats.streaming,true);
const packed=unpackUSDZ(new Uint8Array(Buffer.concat(retainedChunks)));
assert.equal(packed.order[0],'scene/root-converted-1.usdc');
assert.deepEqual(packed.entries.get('scene/root.usdc'),colliding.get('scene/root.usdc'));
assert.deepEqual(packed.entries.get('unreferenced.exr'),new Uint8Array([1,2,3]));
await assert.rejects(convertFolderToUSDZ(native,new Map([...input,['../unsafe.usda',encode('#usda 1.0')]]),
 {rootPath:'scene/root.usda',flatten:false}),/Unsafe retained-layer archive path/);
for (const streaming of [false, true]) {
  for (const rootLayerFormat of ['usda', 'usdc']) {
    const options = {rootPath:'scene/root.usda', rootLayerFormat, reencode:false,
      variantSelections:{'/Root{look}':'blue'}};
    const source = {keys:[...input.keys()], fetch:async key => {
      assert.ok(input.has(key), key);
      return input.get(key);
    }};
    const result = streaming ? await convertSourceToUSDZStreaming(native, source, options)
      : await convertFolderToUSDZ(native, input, options);
    assert.equal(result.stats.flatten, true);
    assert.equal(result.stats.singleLayerRewrite, false);
    const text = textFromPackage(result.usdz);
    assert.match(text, /float gain = 2/);
    assert.doesNotMatch(text, /variantSet |references =|subLayers = \[\s*@/);
    for (const path of ['weak/tex.png', 'red/tex.png', 'blue/tex.png', 'red/alt.png', 'blue/alt.png'])
      assert.ok(text.includes('@' + path + '@'), text);
    assert.doesNotMatch(text, /red\/red\/|blue\/blue\//);
    const archive = unpackUSDZ(result.usdz);
    assert.equal(archive.order[0], 'root.' + rootLayerFormat);
    assert.deepEqual(archive.entries.get('red/tex.png'), new Uint8Array([2]));
    assert.deepEqual(archive.entries.get('blue/tex.png'), new Uint8Array([4]));
    assert.equal([...archive.entries.keys()].filter(name => /\.usd[ac]?$/.test(name)).length, 1);
  }
}
// Missing dependencies must fail before a sink receives a partial package.
const missing = new Map(input);missing.delete('blue/model.usda');
const partial = [];
await assert.rejects(convertFolderToUSDZ(native, missing, {
  rootPath:'scene/root.usda', reencode:false, zipSink:chunk => partial.push(chunk),
}), /Missing USD dependency layer: blue\/model.usda/);
assert.equal(partial.length, 0);
await assert.rejects(convertFolderToUSDZ(native, input, {
  rootPath:'scene/root.usda', reencode:false, maxMemory:64,
}), /input byte limit|memory limit/i);
// Default CLI/UI image settings have no work to do on a texture-free scene.
const noImages = await convertFolderToUSDZ(native,
  new Map([['root.usda', encode('#usda 1.0\ndef Xform "Root" {}')]]),
  {reencode:true, maxTextureSize:512, textureFormat:'png', targetTextureBytes:100,
   textureProcessor:() => {throw new Error('Unexpected image processing');}});
assert.equal(noImages.stats.textures,0);
assert.match(textFromPackage(noImages.usdz), /def Xform "Root"/);
// A missing texture in one directory cannot bind a same-named root texture.
const shadow = new Map([
  ['root.usda', encode('#usda 1.0\ndef Xform "Root" (prepend references=@red/model.usda@</Asset>) {}')],
  ['red/model.usda', encode(dependency)], ['tex.png', new Uint8Array([9])],
]);
const shadowText = textFromPackage((await convertFolderToUSDZ(native, shadow, {reencode:false})).usdz);
assert.match(shadowText, /@red\/tex.png@/);
// Both optimization aliases reach configured core export, including the
// single-USDZ passthrough shortcut that used to silently ignore them.
const geometry = new Uint8Array(fs.readFileSync(new URL('../../../tests/usda/geometry-optimize-nonuniform-normal-001.usda', import.meta.url)));
for (const packed of [false, true]) {
  const key = packed ? 'source.usdz' : 'source.usda';
  const bytes = packed ? buildUSDZWithNewRoot('scene.usda', geometry, []) : geometry;
  const result = await convertFolderToUSDZ(native, new Map([[key, bytes]]), {
    flatten:false, reencode:false, optimizeMeshes:'mergeMeshes', rootLayerFormat:'usda',
  });
  assert.equal(result.stats.flatten, true);
  assert.equal((textFromPackage(result.usdz).match(/def Mesh /g) || []).length, 1);
}
// Disabling a legacy repack optimization must not discard packed assets.
const textured = buildUSDZWithNewRoot('scene.usda',
  encode('#usda 1.0\ndef Shader "Texture" {asset inputs:file=@tex.png@}'),
  [{name:'tex.png',data:new Uint8Array([7,8,9])}]);
const unpackedResult = await convertFolderToUSDZ(native, new Map([['input.usdz',textured]]), {
  repackUsdz:false, flatten:true, reencode:false,
});
assert.deepEqual(unpackUSDZ(unpackedResult.usdz).entries.get('tex.png'),new Uint8Array([7,8,9]));
assert.match(textFromPackage(unpackedResult.usdz), /@tex.png@/);
const material = encode(`#usda 1.0
def Xform "Root" {
 def Material "A" {def Shader "Surface" {uniform token info:id="UsdPreviewSurface"; float inputs:roughness=0.5}}
 def Material "B" {def Shader "Surface" {uniform token info:id="UsdPreviewSurface"; float inputs:roughness=0.5}}
 def Mesh "Mesh" {rel material:binding=</Root/B>}
}`);
const chunks = [];
const sinkResult = await convertFolderToUSDZ(native, new Map([['scene.usda', material]]), {
  reencode:false, materialOptimization:'dedupe', rootLayerFormat:'usda',
  zipSink:chunk => chunks.push(chunk.slice()),
});
assert.equal(sinkResult.streamedToSink, true);assert.equal(sinkResult.usdz, null);
const sinkBytes = new Uint8Array(chunks.reduce((n,c) => n+c.length,0));
let offset=0;for(const chunk of chunks){sinkBytes.set(chunk,offset);offset+=chunk.length;}
const optimized = textFromPackage(sinkBytes);
assert.equal((optimized.match(/def Material /g) || []).length,1,optimized);
assert.match(optimized, /rel material:binding = <\/Root\/A>/);
console.log('PASS: core JS composition, variants, anchored assets/samples, optimization, sinks and failure limits');
