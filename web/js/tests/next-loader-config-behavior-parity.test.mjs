// SPDX-License-Identifier: Apache-2.0
// Paired legacy/next behavior for the loader-configuration family: every
// setting's default, canonical round trip, and conversion effect runs against
// the combined legacy loader and the next-only RenderStream. Legacy Embind
// setters coerce arbitrary JS values; next validates them, so one
// non-canonical input per setter is pinned on both sides as an edge delta.
// Rows naming this test in next-wasm-parity-gaps.json must match what it
// exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-loader-config-behavior-parity.test.mjs';
const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const combinedUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                   : '../src/lightusd/lightusd_combined.js', import.meta.url).href;
const legacyModule = await loadWasm(() => import(combinedUrl));
const {default: createNext} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const nextModule = await createNext();
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const nextName = new Map(matrix.map(row => [row.legacyMethod, row.nextEquivalent || row.legacyMethod]));
const encode = text => new TextEncoder().encode(text);

const exercised = new Map();  // legacy method -> 'match' | 'delta'
const note = (method, kind) => {
  if (exercised.get(method) !== 'delta') exercised.set(method, kind);
};
const outcome = fn => {
  try {
    const value = fn();
    return {value: typeof value === 'number' && Number.isNaN(value) ? 'NaN' : value};
  } catch (error) { return {throws: error.constructor.name}; }
};
const call = (target, method, args, legacy) =>
  outcome(() => target[legacy ? method : nextName.get(method)](...args));

// ------------------------------------------------------------ settings
const settings = [
  // [setter, getter, canonical values, non-canonical input]
  ['setDeferTangentComputation', 'getDeferTangentComputation', [false, true], 1],
  ['setEnableBoneReduction', 'getEnableBoneReduction', [true, false], 1],
  ['setEnableValueClips', 'getEnableValueClips', [false, true], 1],
  ['setRoundBoneCount', 'getRoundBoneCount', [true, false], 1],
  ['setValueClipUseTimeRange', 'getValueClipUseTimeRange', [true, false], 1],
  ['setSphereSubdivisions', 'getSphereSubdivisions', [0, 3, 6], 7],
  ['setTargetBoneCount', 'getTargetBoneCount', [1, 3, 128, 4], 2.7],
  ['setValueClipSampleRate', 'getValueClipSampleRate', [24, 29.97, 0], -1],
];
for (const [setter, getter, values, loose] of settings) {
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    assert.deepEqual(call(stream, getter, [], false), call(loader, getter, [], true),
      `${getter} default must match legacy`);
    for (const value of values) {
      assert.deepEqual(call(stream, setter, [value], false), call(loader, setter, [value], true),
        `${setter}(${value})`);
      assert.deepEqual(call(stream, getter, [], false), call(loader, getter, [], true),
        `${getter} after ${setter}(${value})`);
    }
    note(getter, 'match');
    // Legacy coerces (or silently ignores) the loose input; next throws and
    // keeps its previous value.
    const before = call(stream, getter, [], false);
    assert.equal(call(loader, setter, [loose], true).throws, undefined,
      `legacy ${setter} accepts ${loose}`);
    assert.ok(call(stream, setter, [loose], false).throws, `next ${setter} rejects ${loose}`);
    assert.deepEqual(call(stream, getter, [], false), before);
    exercised.set(setter, 'delta');
  } finally { loader.delete(); stream.delete(); }
}
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    for (const getter of ['getValueClipStartTime', 'getValueClipEndTime'])
      assert.deepEqual(call(stream, getter, [], false), call(loader, getter, [], true));
    for (const range of [[1, 10], [10, 1], [-2.5, 3]]) {
      assert.deepEqual(call(stream, 'setValueClipTimeRange', range, false),
        call(loader, 'setValueClipTimeRange', range, true));
      for (const getter of ['getValueClipStartTime', 'getValueClipEndTime'])
        assert.deepEqual(call(stream, getter, [], false), call(loader, getter, [], true));
    }
    note('setValueClipTimeRange', 'match');
    note('getValueClipStartTime', 'match');
    note('getValueClipEndTime', 'match');
    // Legacy stores non-finite bounds; next rejects them.
    assert.equal(call(loader, 'setValueClipTimeRange', [0, Infinity], true).throws, undefined);
    assert.ok(call(stream, 'setValueClipTimeRange', [0, Infinity], false).throws);
    exercised.set('setValueClipTimeRange', 'delta');

    // Defaults recorded as known gaps: UDIM combining and the memory limit.
    assert.equal(loader.getCombineUDIMTiles(), true);
    assert.equal(stream.getCombineUDIMTiles(), false);
    exercised.set('getCombineUDIMTiles', 'delta');
    loader.setCombineUDIMTiles(false);
    stream.setCombineUDIMTiles(false);
    assert.equal(loader.getCombineUDIMTiles(), stream.getCombineUDIMTiles());
    assert.equal(call(loader, 'setCombineUDIMTiles', [1], true).throws, undefined);
    assert.ok(call(stream, 'setCombineUDIMTiles', [1], false).throws);
    exercised.set('setCombineUDIMTiles', 'delta');
    assert.equal(loader.getMaxMemoryLimitMB(), wasm64 ? 8192 : 2048);
    assert.equal(stream.getMaxMemoryLimitMB(), 1024);
    exercised.set('getMaxMemoryLimitMB', 'delta');
    loader.setMaxMemoryLimitMB(512);
    stream.setMaxMemoryLimitMB(512);
    assert.equal(loader.getMaxMemoryLimitMB(), stream.getMaxMemoryLimitMB());
    assert.equal(call(loader, 'setMaxMemoryLimitMB', [0], true).throws, undefined);
    assert.ok(call(stream, 'setMaxMemoryLimitMB', [0], false).throws);
    exercised.set('setMaxMemoryLimitMB', 'delta');

    // Boolean render-optimization setters share the same coercion delta.
    for (const setter of ['setNativeFlattenRenderTree', 'setNativeMaterialDedup',
      'setNativeMeshMerge', 'setNativeMeshMergeBakeTransform', 'setEnableComposition',
      'setLoadTextureInNative']) {
      for (const value of [true, false])
        assert.deepEqual(call(stream, setter, [value], false), call(loader, setter, [value], true));
      assert.equal(call(loader, setter, [1], true).throws, undefined);
      assert.ok(call(stream, setter, [1], false).throws, `next ${setter} rejects 1`);
      exercised.set(setter, 'delta');
    }
    // debugLogMemory: same record shape; heap sizes are product-specific.
    const legacyLog = loader.debugLogMemory('probe');
    const nextLog = stream.debugLogMemory('probe');
    assert.deepEqual(Object.keys(nextLog).sort(), Object.keys(legacyLog).sort());
    assert.equal(nextLog.label, legacyLog.label);
    assert.ok(nextLog.heapBytes > 0 && legacyLog.heapBytes > 0);
    note('debugLogMemory', 'match');
  } finally { loader.delete(); stream.delete(); }
}

// ------------------------------------------------------------- effects
function loadBoth(source, configure, {legacyAssets = {}, nextStore} = {}) {
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  configure(loader, true);
  configure(stream, false);
  for (const [name, bytes] of Object.entries(legacyAssets)) {
    loader.setAsset(name, bytes);
    if (!nextStore) stream.provideAsset(name, bytes);
  }
  if (nextStore) stream.setAssetStore(nextStore);
  assert.equal(loader.loadFromBinary(encode(source), 'scene.usda'), true, loader.error());
  const loaded = stream.begin(encode(source), 'scene.usda');
  assert.equal(loaded.success, true, loaded.error || stream.error());
  return {loader, stream, done() { loader.delete(); stream.delete(); }};
}
const set = (target, legacy, method, value) =>
  target[legacy ? method : nextName.get(method)](value);
const triangles = (target, legacy, id) => legacy
  ? target.getMesh(id).faceVertexIndices.length / 3
  : target.getMeshCopy(id).points.length / 9;

// Sphere tessellation.
for (const level of [1, 3]) {
  const scene = loadBoth('#usda 1.0\ndef Sphere "Ball" {\n double radius = 2\n}\n',
    (target, legacy) => set(target, legacy, 'setSphereSubdivisions', level));
  try {
    assert.equal(triangles(scene.stream, false, 0), triangles(scene.loader, true, 0));
    assert.equal(triangles(scene.stream, false, 0), 20 * 4 ** level);
  } finally { scene.done(); }
}
note('setSphereSubdivisions', 'match');

// Skin influence reduction, target count and rounding.
const identity = '((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))';
const quad = (name, extra = '', x = 0) => `def Mesh "${name}" {
 int[] faceVertexCounts = [4]
 int[] faceVertexIndices = [0,1,2,3]
 point3f[] points = [(${x},0,0),(${x + 1},0,0),(${x + 1},1,0),(${x},1,0)]
 ${extra}
}`;
const skinSource = `#usda 1.0
def SkelRoot "R" {
 def Skeleton "Skel" {
  uniform token[] joints = ["a","a/b","a/b/c","a/b/c/d","a/b/c/d/e","a/b/c/d/e/f"]
  uniform matrix4d[] bindTransforms = [${Array(6).fill(identity).join(',')}]
  uniform matrix4d[] restTransforms = [${Array(6).fill(identity).join(',')}]
 }
 def Xform "G" (prepend apiSchemas = ["SkelBindingAPI"]) {
  rel skel:skeleton = </R/Skel>
  ${quad('M', `int[] primvars:skel:jointIndices = [${Array(4).fill('0,1,2,3,4,5').join(',')}] (interpolation = "vertex" elementSize = 6)
 float[] primvars:skel:jointWeights = [${Array(4).fill('0.3,0.25,0.2,0.1,0.1,0.05').join(',')}] (interpolation = "vertex" elementSize = 6)`)}
 }
}
`;
for (const [reduce, target, round] of [[false, 4, false], [true, 4, false], [true, 3, false],
  [true, 2, false], [true, 2, true]]) {
  const scene = loadBoth(skinSource, (object, legacy) => {
    set(object, legacy, 'setEnableBoneReduction', reduce);
    set(object, legacy, 'setTargetBoneCount', target);
    set(object, legacy, 'setRoundBoneCount', round);
  });
  try {
    const legacyMesh = scene.loader.getMesh(0);
    const nextMesh = scene.stream.getMeshCopy(0);
    assert.equal(nextMesh.elementSize, legacyMesh.elementSize,
      `influence width reduce=${reduce} target=${target} round=${round}`);
    const round3 = values => Array.from(values, v => Math.round(v * 1e5) / 1e5);
    assert.deepEqual(round3(nextMesh.jointWeights), round3(legacyMesh.jointWeights));
    assert.deepEqual(Array.from(nextMesh.jointIndices), Array.from(legacyMesh.jointIndices));
  } finally { scene.done(); }
}
for (const method of ['setEnableBoneReduction', 'setTargetBoneCount', 'setRoundBoneCount'])
  note(method, 'match');

// Value clips: enablement, resampling rate and time range.
const clipRoot = ['#usda 1.0', 'def Xform "World" {', '  def Xform "Mover" ( clips = {',
  '    dictionary default_clip = {', '      double2[] active = [(0, 0), (1, 1)]',
  '      asset[] assetPaths = [@wide.usda@, @tall.usda@]', '      string primPath = "/World/Mover"',
  '      double2[] times = [(0, 0), (1, 1)]', '    }', '  } ) {',
  '    double3 xformOp:translate = (0, 0, 0)', '    uniform token[] xformOpOrder = ["xformOp:translate"]',
  '  }', '}'].join('\n');
const clip = x => encode(['#usda 1.0', 'def Xform "World" {', '  def Xform "Mover" {',
  `    double3 xformOp:translate = (${x}, 0, 0)`, '    uniform token[] xformOpOrder = ["xformOp:translate"]',
  '  }', '}'].join('\n'));
for (const [enabled, sampled] of [[true, false], [true, true], [false, false]]) {
  const scene = loadBoth(clipRoot, (object, legacy) => {
    set(object, legacy, 'setEnableValueClips', enabled);
    if (sampled) {
      set(object, legacy, 'setValueClipSampleRate', 2);
      set(object, legacy, 'setValueClipUseTimeRange', true);
      object.setValueClipTimeRange(0, 1);
    }
  }, {legacyAssets: {'wide.usda': clip(0), 'tall.usda': clip(10)}});
  try {
    assert.equal(scene.stream.numAnimations(), scene.loader.numAnimations());
    if (enabled) {
      const sampler = object => {
        const {times, values} = object.getAnimation(0).samplers[0];
        return {times: Array.from(times), values: Array.from(values)};
      };
      assert.deepEqual(sampler(scene.stream), sampler(scene.loader));
      assert.deepEqual(sampler(scene.stream).times, sampled ? [0, 0.5, 1] : [0, 1]);
    }
  } finally { scene.done(); }
}
for (const method of ['setEnableValueClips', 'setValueClipSampleRate', 'setValueClipUseTimeRange'])
  note(method, 'match');

// Tangents: only normal-mapped meshes, eagerly or via computeMeshTangents.
const shadedMesh = (name, x, material) => `def Mesh "${name}" (prepend apiSchemas = ["MaterialBindingAPI"]) {
 int[] faceVertexCounts = [4]
 int[] faceVertexIndices = [0,1,2,3]
 point3f[] points = [(${x},0,0),(${x + 1},0,0),(${x + 1},1,0),(${x},1,0)]
 texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
 normal3f[] normals = [(0,0,1),(0,0,1),(0,0,1),(0,0,1)] (interpolation = "vertex")
 rel material:binding = </Looks/${material}>
}`;
const previewMaterial = (name, body) => `def Material "${name}" {
  token outputs:surface.connect = </Looks/${name}/S.outputs:surface>
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   ${body}
   token outputs:surface
  }
 }`;
const tangentSource = `#usda 1.0
def Scope "Looks" {
 ${previewMaterial('Plain', 'color3f inputs:diffuseColor = (1, 0, 0)')}
 def Material "Bumpy" {
  token outputs:surface.connect = </Looks/Bumpy/S.outputs:surface>
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   normal3f inputs:normal.connect = </Looks/Bumpy/N.outputs:rgb>
   token outputs:surface
  }
  def Shader "N" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @normal.png@
   float3 outputs:rgb
  }
 }
}
${shadedMesh('A', 0, 'Plain')}
${shadedMesh('B', 3, 'Bumpy')}
`;
const tangents = (object, legacy, id) => {
  const values = legacy ? object.getMesh(id).tangents : object.meshTangentsBuffer(id);
  return values && values.length ? Array.from(values.subarray(0, 4)) : null;
};
for (const defer of [true, false]) {
  const scene = loadBoth(tangentSource, (object, legacy) => {
    set(object, legacy, 'setDeferTangentComputation', defer);
    set(object, legacy, 'setNativeMeshMerge', false);
  });
  try {
    for (const id of [0, 1])
      assert.deepEqual(tangents(scene.stream, false, id), tangents(scene.loader, true, id),
        `eager tangents defer=${defer} mesh=${id}`);
    assert.equal(tangents(scene.stream, false, 1) !== null, !defer);
    for (const id of [0, 1, 2])
      assert.equal(scene.stream.computeMeshTangents(id), scene.loader.computeMeshTangents(id));
    for (const id of [0, 1])
      assert.deepEqual(tangents(scene.stream, false, id), tangents(scene.loader, true, id),
        `requested tangents defer=${defer} mesh=${id}`);
    // Legacy re-indexes a mesh when it computes deferred tangents, so only
    // the per-vertex values (not the stream length) are compared above.
  } finally { scene.done(); }
}
note('setDeferTangentComputation', 'delta');
note('computeMeshTangents', 'match');

// Mesh merge, bake transform (legacy default: bake), and material dedup.
const looks = `def Scope "Looks" {
 ${previewMaterial('M1', 'color3f inputs:diffuseColor = (1, 0, 0)')}
 ${previewMaterial('M2', 'color3f inputs:diffuseColor = (1, 0, 0)')}
 ${previewMaterial('M3', 'color3f inputs:diffuseColor = (0, 0, 1)')}
 ${previewMaterial('Unused', 'color3f inputs:diffuseColor = (0, 1, 0)')}
}`;
const placed = (name, x, material) => `def Xform "X${name}" {
 double3 xformOp:translate = (${x}, 0, 5)
 uniform token[] xformOpOrder = ["xformOp:translate"]
 def Mesh "${name}" (prepend apiSchemas = ["MaterialBindingAPI"]) {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0,1,2]
  point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
  ${material ? `rel material:binding = </Looks/${material}>` : ''}
 }
}`;
const mergeSource = `#usda 1.0\n${looks}\n${placed('A', 0, 'M3')}\n${placed('B', 3, 'M1')}\n${placed('C', 6, 'M2')}\n`;
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    for (const getter of ['getNativeMeshMerge', 'getNativeMaterialDedup',
      'getNativeMeshMergeBakeTransform', 'getNativeFlattenRenderTree'])
      assert.equal(stream[getter.replace('Native', '')](), loader[getter](), `${getter} default`);
  } finally { loader.delete(); stream.delete(); }
}
const meshSummary = (object, legacy) => {
  const out = [];
  for (let id = 0; id < object.numMeshes(); ++id) {
    const mesh = legacy ? object.getMesh(id) : object.getMeshCopy(id);
    out.push({name: mesh.primName, material: mesh.materialId,
      origin: Array.from(mesh.points.subarray(0, 3))});
  }
  return {materials: object.numMaterials(), meshes: out};
};
for (const [merge, dedup, bake] of [[false, false, true], [false, true, true], [true, true, false],
  [true, true, true], [true, false, true]]) {
  const scene = loadBoth(mergeSource, (object, legacy) => {
    set(object, legacy, 'setNativeMeshMerge', merge);
    set(object, legacy, 'setNativeMaterialDedup', dedup);
    set(object, legacy, 'setNativeMeshMergeBakeTransform', bake);
  });
  try {
    assert.deepEqual(meshSummary(scene.stream, false), meshSummary(scene.loader, true),
      `merge=${merge} dedup=${dedup} bake=${bake}`);
    // Material ids index the records returned for them, independent of the
    // order in which meshes are read.
    if (!merge) {
      const expected = {A: '/Looks/M3', B: '/Looks/M1', C: dedup ? '/Looks/M1' : '/Looks/M2'};
      for (let id = scene.stream.numMeshes() - 1; id >= 0; --id) {
        const mesh = scene.stream.getMeshCopy(id);
        assert.equal(scene.stream.getMaterialRecord(mesh.materialId).primPath, expected[mesh.primName]);
      }
    }
  } finally { scene.done(); }
}
note('setNativeMeshMerge', 'match');
note('setNativeMaterialDedup', 'match');
note('setNativeMeshMergeBakeTransform', 'match');
note('numMaterials', 'match');
// Unbound meshes: legacy reports material -1; next points them at a trailing
// fallback record whose id equals numMaterials.
{
  const scene = loadBoth(`#usda 1.0\n${looks}\n${placed('A', 0, 'M1')}\n${placed('D', 3, null)}\n`,
    () => {});
  try {
    assert.equal(scene.loader.getMesh(1).materialId, -1);
    assert.equal(scene.stream.getMeshCopy(1).materialId, scene.stream.numMaterials());
    assert.equal(scene.stream.numMaterials(), scene.loader.numMaterials());
  } finally { scene.done(); }
  exercised.set('numMaterials', 'delta');
}

// Flattened render tree.
const treeSource = `#usda 1.0
def Xform "Root" {
 def Xform "Mid" {
  double3 xformOp:translate = (1, 0, 0)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  def Scope "Group" {
   ${quad('Tri')}
  }
 }
}
`;
const tree = node => ({name: node.primName, path: node.absPath,
  matrix: Array.from(node.localMatrix), children: (node.children || []).map(tree)});
for (const flatten of [false, true]) {
  const scene = loadBoth(treeSource,
    (object, legacy) => set(object, legacy, 'setNativeFlattenRenderTree', flatten));
  try {
    assert.deepEqual(tree(scene.stream.getDefaultRootNode()), tree(scene.loader.getDefaultRootNode()),
      `flattenRenderTree=${flatten}`);
  } finally { scene.done(); }
}
note('setNativeFlattenRenderTree', 'match');

// Native texture decoding: legacy reads setAsset bytes, next an attached
// NextAssetStore. Next expands RGB to RGBA and reports unknown sizes as 0.
function onePixelPNG() {
  const table = Array.from({length: 256}, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc = bytes => {
    let c = 0xffffffff;
    for (const b of bytes) c = table[(c ^ b) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const chunk = (type, data) => {
    const head = Buffer.alloc(4); head.writeUInt32BE(data.length);
    const tail = Buffer.alloc(4); tail.writeUInt32BE(crc(Buffer.concat([Buffer.from(type), data])));
    return Buffer.concat([head, Buffer.from(type), data, tail]);
  };
  const header = Buffer.alloc(13);
  header.writeUInt32BE(1, 0); header.writeUInt32BE(1, 4); header[8] = 8; header[9] = 2;
  return new Uint8Array(Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(Buffer.from([0, 255, 0, 0]))),
    chunk('IEND', Buffer.alloc(0))]));
}
const textureSource = `#usda 1.0
def Scope "Looks" {
 def Material "M" {
  token outputs:surface.connect = </Looks/M/S.outputs:surface>
  def Shader "S" {
   uniform token info:id = "UsdPreviewSurface"
   color3f inputs:diffuseColor.connect = </Looks/M/T.outputs:rgb>
   token outputs:surface
  }
  def Shader "T" {
   uniform token info:id = "UsdUVTexture"
   asset inputs:file = @red.png@
   float3 outputs:rgb
  }
 }
}
${placed('Q', 0, 'M')}
`;
for (const native of [false, true]) {
  const store = new nextModule.NextAssetStore();
  store.setAsset('red.png', onePixelPNG());
  const scene = loadBoth(textureSource,
    (object, legacy) => set(object, legacy, 'setLoadTextureInNative', native),
    {legacyAssets: {'red.png': onePixelPNG()}, nextStore: store});
  try {
    const legacyImage = scene.loader.getImage(0);
    const nextImage = scene.stream.getImageCopy(0);
    assert.equal(nextImage.decoded, legacyImage.decoded);
    assert.equal(nextImage.uri, legacyImage.uri);
    if (native) {
      assert.deepEqual([nextImage.width, nextImage.height], [legacyImage.width, legacyImage.height]);
      assert.deepEqual([legacyImage.channels, nextImage.channels], [3, 4]);
    } else {
      assert.deepEqual([legacyImage.width, nextImage.width], [-1, 0]);
    }

    // Memory statistics: resource counts match; node, buffer and cache-byte
    // fields report each product's own accounting.
    const legacyStats = scene.loader.getMemoryStats();
    const nextStats = scene.stream.getMemoryStats();
    assert.deepEqual(Object.keys(nextStats).sort(), Object.keys(legacyStats).sort());
    for (const key of ['numMeshes', 'numMaterials', 'numTextures', 'numImages', 'numLights',
      'assetCacheCount'])
      assert.equal(nextStats[key], legacyStats[key], `getMemoryStats().${key}`);
  } finally { scene.done(); store.delete(); }
}
exercised.set('setLoadTextureInNative', 'delta');
exercised.set('getMemoryStats', 'delta');

// Composition: legacy stores the flag but never composes on load; next does.
{
  const source = `#usda 1.0\ndef Xform "Proto" {\n ${quad('Q')}\n}\ndef Xform "Inst" (\n references = </Proto>\n) {\n}\n`;
  for (const [enabled, legacyMeshes, nextMeshes] of [[true, 1, 2], [false, 1, 1]]) {
    const scene = loadBoth(source,
      (object, legacy) => set(object, legacy, 'setEnableComposition', enabled));
    try {
      assert.equal(scene.loader.numMeshes(), legacyMeshes);
      assert.equal(scene.stream.numMeshes(), nextMeshes);
    } finally { scene.done(); }
  }
}

// USDC export limits: legacy applies them to the loader's exportAsUSDC, next
// to NextUSDZConverterNative. The file-size cap matches; next also enforces
// the memory cap as an up-front working-set estimate that legacy does not.
{
  let seed = 0x12345678;
  const alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_';
  const label = Array.from({length: 1800}, () => {
    let part = '';
    for (let i = 0; i < 1000; ++i) {
      seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
      part += alphabet[(seed >>> 0) & 63];
    }
    return part;
  }).join('');
  const source = encode(`#usda 1.0\ndef Xform "Root" {\n custom string payload = "${label}"\n}\n`);
  const loader = new legacyModule.LightUSDLoaderNative();
  const converter = new nextModule.NextUSDZConverterNative();
  try {
    assert.equal(loader.loadFromBinary(source, 'large.usda'), true, loader.error());
    assert.equal(converter.loadFromBinary(source, 'large.usda'), true, converter.error());
    const exported = target => target.exportAsUSDC() instanceof Uint8Array;
    for (const [fileMB, memoryMB] of [[0, 0], [1, 0], [2, 0], [2, 4]]) {
      loader.setUSDCExportLimitMB(fileMB, memoryMB);
      converter.setUSDCExportLimitMB(fileMB, memoryMB);
      assert.equal(exported(converter), exported(loader), `USDC limit ${fileMB}/${memoryMB} MB`);
    }
    assert.equal(exported(loader), true);
    loader.setUSDCExportLimitMB(2, 1);
    converter.setUSDCExportLimitMB(2, 1);
    assert.deepEqual([exported(loader), exported(converter)], [true, false]);
    assert.match(converter.error(), /estimated working set/);
  } finally { loader.delete(); converter.delete(); }
  exercised.set('setUSDCExportLimitMB', 'delta');
}

// ---------------------------------------------------------- matrix sync
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...exercised.keys()].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  const kind = exercised.get(row.legacyMethod);
  if (row.parityStatus === 'behavior_verified') {
    assert.equal(Boolean(row.edgeDifference), kind === 'delta',
      `${row.legacyMethod}: edgeDifference must be documented exactly when a delta is pinned`);
  } else {
    assert.equal(row.parityStatus, 'known_behavior_gap', row.legacyMethod);
    assert.equal(kind, 'delta', `${row.legacyMethod}: a known gap must pin its delta`);
  }
}
const gaps = claimed.filter(row => row.parityStatus === 'known_behavior_gap').length;
console.log(`ok - loader configuration behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods verified against legacy, ${gaps} documented gaps`);
