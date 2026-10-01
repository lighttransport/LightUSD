// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {PNG} from 'pngjs';
import {bakeUDIMAtlas, discoverUDIMTiles, splitUDIMPattern, udimOptions} from '../src/udim-bake.js';
import {loadWasm} from '../src/usdzconvert.js';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const combined = process.env.LIGHTUSD_UDIM_COMBINED === '1';
const glue = process.env.LIGHTUSD_UDIM_GLUE || (combined ? (wasm64 ? '../src/lightusd/lightusd_combined_64.js' : '../src/lightusd/lightusd_combined.js') : wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js');
const dir = new URL('.', new URL(glue, import.meta.url));
const native = await loadWasm(() => import(glue), {locateFile: file => new URL(file, dir).href});
function solid(r, g, b, a = 255) {
  const png = new PNG({width: 2, height: 2});
  for (let i = 0; i < png.data.length; i += 4) png.data.set([r, g, b, a], i);
  return new Uint8Array(PNG.sync.write(png));
}
const map = new Map([
  ['textures/tile.1001.png', solid(255, 0, 0)],
  ['textures/tile.1002.png', solid(0, 255, 0)],
  ['textures/tile.1101.png', solid(0, 0, 255)],
  ['textures/unrelated.png', solid(255, 0, 255)],
  ['textures/frame.1003.png', solid(255, 255, 0)],
]);
const source = {keys: [...map.keys()], fetch: async key => map.get(key)};
assert.deepEqual(splitUDIMPattern('x.%04d.png'), {prefix: 'x.', suffix: '.png'});
assert.equal(splitUDIMPattern('x.<UDIM>.%04d.png'), null);
assert.throws(() => udimOptions({udimMaxTiles: 0}), /udimMaxTiles/);
assert.throws(() => discoverUDIMTiles(source.keys, 'textures/tile.<UDIM>.png', '', 2), /tile count/);
for (const mode of ['grid', 'dense']) {
  const result = await bakeUDIMAtlas(native, source, 'textures/tile.<UDIM>.png', {udimBake: mode});
  assert.equal(result.tiles.length, 3);
  assert.match(result.name, /^textures\/udim_[0-9a-f]{64}\.png$/);
  const png = PNG.sync.read(Buffer.from(result.data));
  assert.equal(png.width, result.layout.width);
  assert.equal(png.height, result.layout.height);
  for (const cell of result.layout.cells) {
    const x = cell.x * (result.layout.tileWidth + 2 * result.layout.padding) + result.layout.padding;
    const y = png.height - (cell.y + 1) * (result.layout.tileHeight + 2 * result.layout.padding) + result.layout.padding;
    const actual = [...png.data.subarray((y * png.width + x) * 4, (y * png.width + x) * 4 + 4)];
    const expected = cell.id === 1001 ? [255, 0, 0, 255] : cell.id === 1002 ? [0, 255, 0, 255] : [0, 0, 255, 255];
    assert.deepEqual(actual, expected, `${mode}: tile ${cell.id} orientation and color`);
  }
}
await assert.rejects(() => bakeUDIMAtlas(native, source, 'textures/tile.<UDIM>.png', {udimBake: 'grid', udimMaxTiles: 2}), /tile count/);
await assert.rejects(() => bakeUDIMAtlas(native, source, 'textures/tile.<UDIM>.png', {udimBake: 'grid', udimMemoryBudgetBytes: 128}), /memory/);
await assert.rejects(() => bakeUDIMAtlas(native, source, 'textures/tile.<UDIM>.png', {udimBake: 'grid', udimMaxAtlasSize: 8}), /size limit/);
// A rejected job must release its handle and memory so another bake succeeds.
await bakeUDIMAtlas(native, source, 'textures/tile.<UDIM>.png', {udimBake: 'grid'});
const samples16 = new Uint16Array([12345,23456,34567,65535]);
const input16 = PNG.sync.write({width: 1,height: 1,data: new Uint8Array(samples16.buffer)}, {bitDepth: 16,inputColorType: 6,colorType: 6,inputHasAlpha: true});
const atlas16 = await bakeUDIMAtlas(native, {keys: ['tile.1001.png'],fetch: async () => input16}, 'tile.<UDIM>.png', {udimBake: 'grid'}, '', false);
const output16 = PNG.sync.read(Buffer.from(atlas16.data), {skipRescale: true});
assert.equal(output16.depth, 16);assert.deepEqual([...output16.data], [...samples16]);
console.log(`UDIM shared image core: ${wasm64 ? 'memory64' : 'wasm32'} grid/dense passed`);

const {convertFolderToUSDZ, parseUSDZEntries} = await import('../src/usdzconvert.js');
const scene = `#usda 1.0
 def Xform "World" {
  def Mesh "Mesh" {
   point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
   int[] faceVertexCounts = [3]
   int[] faceVertexIndices = [0,1,2]
   uniform token subdivisionScheme = "none"
   texCoord2f[] primvars:st = [(0.1,0.1), (0.8,0.1), (0.1,0.8)] (interpolation = "faceVarying")
   rel material:binding = </World/Mat>
  }
  def Material "Mat" {
   token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
   def Shader "Surface" {
    uniform token info:id = "UsdPreviewSurface"
    color3f inputs:diffuseColor.connect = </World/Mat/Texture.outputs:rgb>
    token outputs:surface
   }
   def Shader "Texture" {
    uniform token info:id = "UsdUVTexture"
    asset inputs:file = @textures/tile.<UDIM>.png@
    float2 inputs:st.connect = </World/Mat/Reader.outputs:result>
    float3 outputs:rgb
   }
   def Shader "Reader" {
    uniform token info:id = "UsdPrimvarReader_float2"
    token inputs:varname = "st"
    float2 outputs:result
   }
  }
 }
`;
for (const mode of ['grid', 'dense']) {
  const input = new Map(map); input.set('root.usda', new TextEncoder().encode(scene));
  const result = await convertFolderToUSDZ(native, input, {flatten: combined, reencode: false, rootLayerFormat: 'usda', udimBake: mode});
  const entries = parseUSDZEntries(result.usdz);
  const root = new TextDecoder().decode(entries[0].data);
  assert.match(root, /udim_[0-9a-f]{64}\.png/);
  assert.doesNotMatch(root, /<UDIM>/);
  assert.match(root, mode === 'grid' ? /UsdTransform2d/ : /primvars:_udimAtlas0/);
  assert.equal(entries.filter(entry => /udim_.*\.png/.test(entry.name)).length, 1);
  assert.equal(result.stats.udimTiles, 3);
}
console.log('UDIM end-to-end layer edits: grid/dense passed');

const {convertSourceToUSDZStreaming} = await import('../src/usdzconvert.js');
async function convertedText(content, extra = {}, streaming = false) {
  const input = new Map(map);input.set('root.usda', new TextEncoder().encode(content));
  const options = {flatten: combined, reencode: false, rootLayerFormat: 'usda', udimBake: 'dense', ...extra};
  const result = streaming ? await convertSourceToUSDZStreaming(native, {keys: [...input.keys()], fetch: async key => input.get(key)}, options)
    : await convertFolderToUSDZ(native, input, options);
  const entries = parseUSDZEntries(result.usdz);
  let text = new TextDecoder().decode(entries[0].data);
  if (entries[0].name.endsWith('.usdc')) {
    const converter = combined ? new native.LightUSDLoaderNative() : new native.NextUSDZConverterNative();
    try {assert.equal(combined ? converter.loadAsLayerFromBinary(entries[0].data, 'root.usdc') : converter.loadFromBinary(entries[0].data, 'root.usdc'), true);text=combined ? converter.layerToString() : converter.exportAsUSDA();}finally{converter.delete();}
  }
  return {text, entries, result};
}
const crossing = scene.replace('(0.8,0.1)', '(1.8,0.1)');
await assert.rejects(() => convertedText(crossing), /crosses tile/);
const withLabels = crossing.replace('   rel material:binding', `   token[] primvars:label = ["surface"] (interpolation = "uniform")
   string[] primvars:tag = ["stable", "stable", "stable"] (interpolation = "vertex")
   rel material:binding`);
const split = await convertedText(withLabels, {udimCrossTile: 'split'});
assert.match(split.text, /primvars:label = \["surface", "surface", "surface"\]/);
assert.match(split.text, /string\[\] primvars:tag/);
assert.match(split.text, /faceVertexCounts = \[3, 3, 3\]/);
assert.match(split.text, /primvars:st/); // Authored UVs survive the clipping.
const animated = crossing.replace('   int[] faceVertexCounts', `   point3f[] points.timeSamples = {
    0: [(0,0,0), (1,0,0), (0,1,0)],
    1: [(0,0,1), (1,0,1), (0,1,1)]
   }
   int[] faceVertexCounts`);
const skinned = animated.replace('   rel material:binding', `   int[] primvars:skel:jointIndices = [0,1, 1,2, 0,2] (interpolation = "vertex" elementSize = 2)
   float[] primvars:skel:jointWeights = [0.75,0.25, 0.5,0.5, 0.25,0.75] (interpolation = "vertex" elementSize = 2)
   rel skel:blendShapeTargets = </World/Shape>
   token[] skel:blendShapes = ["Smile"]
   rel material:binding`).replace('  def Material "Mat"', `  def BlendShape "Shape" {
   int[] pointIndices = [0,2]
   vector3f[] offsets = [(0,0,0.2), (0,0,0.5)]
   vector3f[] inbetweens:half:offsets = [(0,0,0.1), (0,0,0.25)]
   uniform float inbetweens:half:weight = 0.5
  }
  def Material "Mat"`);
const deformed = await convertedText(skinned, {udimCrossTile: 'split'});
assert.match(deformed.text, /points.timeSamples/);
assert.match(deformed.text, /elementSize = 3/);
assert.match(deformed.text, /_udimBlendShape/);
assert.match(deformed.text, /inbetweens:half:offsets/);
const weightMatch=deformed.text.match(/primvars:skel:jointWeights = \[([^\]]+)\]/);
assert.ok(weightMatch);
const weights=weightMatch[1].split(',').map(Number);
assert.equal(weights.length, 27);
for(let i=0;i<weights.length;i+=3)assert.ok(Math.abs(weights[i]+weights[i+1]+weights[i+2]-1)<1e-5);
const subdivision = skinned.replace('subdivisionScheme = "none"', 'subdivisionScheme = "catmullClark"');
const refined = await convertedText(subdivision, {udimCrossTile: 'split', udimSubdivisionLevel: 1});
assert.match(refined.text, /subdivisionScheme = "none"/);
assert.match(refined.text, /points.timeSamples/);
assert.match(refined.text, /_udimBlendShape/);
const stream = await convertedText(scene, {}, true);
assert.match(stream.text, /_udimAtlas0/);
assert.equal(stream.result.stats.udimTiles, 3);
assert.equal(stream.entries.filter(e=>/udim_.*\.png/.test(e.name)).length,1);
await assert.rejects(() => convertedText(scene.replace('(0.8,0.1)', '(0.8,0.1)').replace('   rel material:binding', `   int[] faceVertexIndices.timeSamples = {0: [0,1,2], 1: [0,2,1]}
   rel material:binding`)), /animated topology/);

const movingIndices = scene.replace('   rel material:binding', `   int[] primvars:st:indices = [0,1,2]
   int[] primvars:st:indices.timeSamples = {0: [0,1,2], 1: [1,0,2]}
   rel material:binding`);
await assert.rejects(() => convertedText(movingIndices), /animated primvar indices/);
console.log('UDIM geometry, animation, skins, blendshapes, subdivision, and streaming passed');

const animatedFiles = scene.replace('asset inputs:file = @textures/tile.<UDIM>.png@', `asset inputs:file.timeSamples = {
     0: @textures/tile.<UDIM>.png@,
     1: @textures/frame.<UDIM>.png@
    }`);
for (const mode of ['grid', 'dense']) {
  const result = await convertedText(animatedFiles, {udimBake: mode});
  assert.doesNotMatch(result.text, /<UDIM>/);
  assert.match(result.text, /inputs:file.timeSamples/);
  assert.doesNotMatch(result.text, /asset inputs:file =/); // No default opinion was authored.
  assert.equal(result.entries.filter(e => /udim_.*\.png/.test(e.name)).length, 2);
  assert.equal((result.text.match(new RegExp(mode === 'grid' ? 'info:id = "UsdTransform2d"' : 'token inputs:varname = "_udimAtlas0"', 'g')) || []).length, 1);
  const dimensions = result.entries.filter(e => /udim_.*\.png/.test(e.name)).map(e => PNG.sync.read(Buffer.from(e.data)));
  assert.equal(dimensions[0].width, dimensions[1].width);
  assert.equal(dimensions[0].height, dimensions[1].height);
  await assert.rejects(() => convertedText(animatedFiles, {udimBake: mode, udimMaxTiles: 3}), /tile count/);
}
assert.match((await convertedText(animatedFiles, {}, true)).text, /inputs:file.timeSamples/);
console.log('UDIM animated files: common layouts, blank cells, tile caps, and streaming passed');
const body = scene.slice(scene.indexOf('  def Mesh'), scene.lastIndexOf(' }'));
const variantScene = `#usda 1.0
 def Xform "World" (prepend variantSets = "look" variants = {string look = "a"}) {
  variantSet "look" = {
   "a" {${body}}
   "b" {${body.replace('textures/tile.<UDIM>.png', 'textures/frame.<UDIM>.png')}}
  }
 }
`;
const variants = await convertedText(variantScene, {udimBake: 'grid', flatten: false});
assert.match(variants.text, /variantSet "look"/);
assert.doesNotMatch(variants.text, /<UDIM>/);
assert.equal((variants.text.match(/info:id = "UsdTransform2d"/g) || []).length, 2);
const instancesScene = scene.replace('  def Mesh "Mesh" {', '  def Xform "Prototype" {\n  def Mesh "Mesh" {')
  .replace('   rel material:binding = </World/Mat>', '')
  .replace('  def Material "Mat"', `  }
  def Xform "Instance" (instanceable = true prepend references = </World/Prototype>) {
   rel material:binding = </World/Mat>
  }
  def Material "Mat"`);
const instances = await convertedText(instancesScene);
assert.match(instances.text, /primvars:_udimAtlas0/);
assert.doesNotMatch(instances.text, /<UDIM>/);
if (!combined) assert.match(instances.text, /instanceable = true/);
if (!combined) {
  const override = instancesScene.replace('   rel material:binding', `   over "Mesh" {
    point3f[] points = [(0,0,0), (2,0,0), (0,2,0)]
   }
   rel material:binding`);
  await assert.rejects(() => convertedText(override), /instance geometry overrides require composition/);
}
// A concrete tile shared by another texture consumer must remain packaged.
const concrete = await convertedText(scene.replace('   def Shader "Reader"', `   def Shader "Other" {
    uniform token info:id = "UsdUVTexture"
    asset inputs:file = @textures/tile.1001.png@
   }
   def Shader "Reader"`));
assert.ok(concrete.entries.some(entry => entry.name === 'textures/tile.1001.png'));
console.log('UDIM variants, instance consumers, and shared source tiles passed');
if (combined) {
  const nested = new Map([...map].map(([key,value]) => ['asset/' + key,value]));
  nested.set('asset/scene.usda', new TextEncoder().encode(scene));
  nested.set('root.usda', new TextEncoder().encode('#usda 1.0\ndef Xform "World" (prepend references = @asset/scene.usda@</World>) {}\n'));
  const result = await convertFolderToUSDZ(native, nested, {flatten: false, udimBake: 'grid', reencode: false});
  const entries = parseUSDZEntries(result.usdz);
  const dependency = entries.find(entry => entry.name === 'asset/scene.usda');
  assert.ok(dependency);
  assert.doesNotMatch(new TextDecoder().decode(dependency.data), /<UDIM>/);
  assert.ok(entries.some(entry => /asset\/textures\/udim_.*\.png/.test(entry.name)));
}



if (process.env.LIGHTUSD_NATIVE_USDZCONVERT) {
  const fs = await import('node:fs');const os = await import('node:os');const path = await import('node:path');const {spawnSync} = await import('node:child_process');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lightusd-udim-native-'));
  try {
    fs.mkdirSync(path.join(dir, 'textures'));for(const [key,value] of map)fs.writeFileSync(path.join(dir,key),value);
    for(const [label,content,mode,policy] of [['grid', scene,'grid','reject'],['dense',scene,'dense','reject'],['skin',skinned,'dense','split'],['subdivision',subdivision,'dense','split'],['animated-files',animatedFiles,'dense','reject'],['instances',instancesScene,'dense','reject'],['labels',withLabels,'dense','split']]) {
      const input = path.join(dir,'root.usda');fs.writeFileSync(input,content);
      for(const ext of ['usda','usdc','usdz']) {
        const output=path.join(dir,label+'.'+ext);
        const run=spawnSync(process.env.LIGHTUSD_NATIVE_USDZCONVERT,[input,output,'--outputFormat',ext,'--bake-udim',mode,'--udim-cross-tile',policy,'--udim-subdivision-level','1'],{encoding:'utf8'});
        assert.equal(run.status,0,run.stderr+run.stdout);assert.ok(fs.statSync(output).size>0);
        if(ext==='usda') {const text=fs.readFileSync(output,'utf8');assert.doesNotMatch(text,/<UDIM>/);assert.match(text,/udim_[0-9a-f]{64}\.png/);if(label==='skin'||label==='subdivision')assert.match(text,/_udimBlendShape/);}
        if(ext==='usdz')assert.ok(parseUSDZEntries(new Uint8Array(fs.readFileSync(output))).some(entry=>/udim_[0-9a-f]{64}\.png/.test(entry.name)));
      }
    }
    // Publishing a conflicting sidecar must leave the previous root intact.
    const output = path.join(dir, 'grid.usda');
    const before = fs.readFileSync(output);
    const image = before.toString().match(/@([^@]*udim_[0-9a-f]{64}\.png)@/)[1];
    const sidecar = path.join(dir, image);
    fs.writeFileSync(sidecar, new Uint8Array([1,2,3]));
    fs.writeFileSync(path.join(dir, 'root.usda'), scene);
    const collision = spawnSync(process.env.LIGHTUSD_NATIVE_USDZCONVERT,
      [path.join(dir,'root.usda'),output,'--outputFormat','usda','--bake-udim','grid'],{encoding:'utf8'});
    assert.notEqual(collision.status,0);
    assert.deepEqual(fs.readFileSync(output),before);
    assert.deepEqual([...fs.readFileSync(sidecar)],[1,2,3]);
    assert.ok(!fs.readdirSync(dir).some(name => name.includes('.udim-tmp-')));
    // Non-flattened grid baking visits every authored variant.
    fs.writeFileSync(path.join(dir,'root.usda'),variantScene);
    const variantRun=spawnSync(process.env.LIGHTUSD_NATIVE_USDZCONVERT,
      [path.join(dir,'root.usda'),path.join(dir,'variants.usda'),'--outputFormat','usda','-noFlatten','--bake-udim','grid'],{encoding:'utf8'});
    assert.equal(variantRun.status,0,variantRun.stderr+variantRun.stdout);
    const variantText=fs.readFileSync(path.join(dir,'variants.usda'),'utf8');
    assert.match(variantText,/variantSet "look"/);assert.doesNotMatch(variantText,/<UDIM>/);
    // Relative tiles in a dependency take precedence over root search paths.
    fs.mkdirSync(path.join(dir, 'asset/textures'), {recursive: true});
    fs.writeFileSync(path.join(dir, 'asset/scene.usda'), scene);
    for (const id of [1001, 1002, 1101]) {
      fs.writeFileSync(path.join(dir, `asset/textures/tile.${id}.png`), solid(0, 255, 0));
    }
    fs.writeFileSync(path.join(dir, 'root.usda'), `#usda 1.0
def Shader "RootTexture" {
 uniform token info:id = "UsdUVTexture"
 asset inputs:file = @textures/tile.<UDIM>.png@
 float2 inputs:st = (0, 0)
}
def Xform "World" (prepend references = @asset/scene.usda@</World>) {}
`);
    const nestedOutput = path.join(dir, 'nested.usdz');
    const nestedRun = spawnSync(process.env.LIGHTUSD_NATIVE_USDZCONVERT,
      [path.join(dir, 'root.usda'), nestedOutput, '--outputFormat', 'usdz',
        '-noFlatten', '--bake-udim', 'grid'], {encoding: 'utf8'});
    assert.equal(nestedRun.status, 0, nestedRun.stderr + nestedRun.stdout);
    const nestedEntries = parseUSDZEntries(new Uint8Array(fs.readFileSync(nestedOutput)));
    const dependency = nestedEntries.find(entry => entry.name === 'asset/scene.usda');
    assert.ok(dependency);
    const dependencyText = new TextDecoder().decode(dependency.data);
    const dependencyAtlas = dependencyText.match(/@(textures\/udim_[^@]+)@/)[1];
    const imageEntry = nestedEntries.find(entry => entry.name === dependencyAtlas);
    assert.ok(imageEntry);
    assert.deepEqual([...PNG.sync.read(Buffer.from(imageEntry.data)).data.subarray(0, 4)], [0, 255, 0, 255]);
    console.log('Native CLI: USDA/USDC/USDZ grid, dense, deformation and subdivision passed');
  } finally {fs.rmSync(dir,{recursive:true,force:true});}
}
