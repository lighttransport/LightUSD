// SPDX-License-Identifier: Apache-2.0
// Paired legacy/next behavior for schema and image utilities: URDF/sample
// scene authoring on NextUSDZConverterNative, image encoding and bone
// textures on RenderStream. Encoders differ in container layout, so encoded
// images are compared by their decoded pixels; authored scenes are compared
// as reloaded layers. Rows naming this test in next-wasm-parity-gaps.json
// must match what it exercises.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const TEST_NAME = 'next-schema-utilities-behavior-parity.test.mjs';
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
const encode = text => new TextEncoder().encode(text);

const exercised = new Map();  // method -> 'match' | 'delta'
const note = (method, kind) => {
  if (exercised.get(method) !== 'delta') exercised.set(method, kind);
};
const outcome = fn => {
  try { return {value: fn()}; } catch (error) { return {throws: error.constructor.name}; }
};

function prims(json) {
  const out = {};
  const walk = (specs, base) => {
    for (const [name, prim] of Object.entries(specs || {})) {
      const primPath = `${base}/${name}`;
      const properties = {};
      for (const [prop, record] of Object.entries(prim.properties || {}))
        properties[prop] = record.attribute?.value ?? record.relationship?.targets ?? record.propertyType;
      out[primPath] = {type: prim.typeName, specifier: prim.specifier, properties};
      walk(prim.children, primPath);
    }
  };
  walk(json.primSpecs, '');
  return out;
}
function layer(usda) {
  const document = new nextModule.LayerDocument();
  try {
    assert.equal(document.load(encode(usda)).success, true);
    const json = JSON.parse(document.exportJSON().text);
    return {metas: json.metas, prims: prims(json)};
  } finally { document.delete(); }
}

// --------------------------------------------------- URDF / sample scenes
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const converter = new nextModule.NextUSDZConverterNative();
  const both = fn => [outcome(() => fn(loader)), outcome(() => fn(converter))];
  const positions = () => new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
  const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1]);
  const uv = new Float32Array([0, 0, 1, 0, 0, 1]);
  const indices = new Int32Array([0, 1, 2]);
  const robot = JSON.stringify({name: 'TestBot', upAxis: 'Z', links: [{name: 'base',
    visuals: [{name: 'visual', meshRef: 'tri'}], collisions: [{name: 'collision', meshRef: 'collider'}]}],
    joints: []});
  try {
    let [a, b] = both(target => target.createSampleScene());
    assert.deepEqual(b, a);
    assert.deepEqual(layer(converter.exportAsUSDA()), layer(loader.exportAsUSDA()), 'createSampleScene');
    note('createSampleScene', 'match');

    [a, b] = both(target => target.createURDFPhysicsScene('{bad'));
    assert.deepEqual(b, a);
    // Invalid uploads fail on both; an empty mesh name throws in next.
    for (const args of [['bad', new Float32Array(3), null, null, null],
      ['bad', positions(), new Float32Array(2), null, null],
      ['bad', positions(), null, null, new Int32Array(2)]]) {
      [a, b] = both(target => target.setVisualMesh(...args));
      assert.deepEqual(b, a);
    }
    [a, b] = both(target => target.setVisualMesh('', positions(), null, null, null));
    assert.deepEqual([a, b], [{value: false}, {value: false}], 'empty mesh name');
    // A null positions array: legacy reports false, next rejects the argument.
    [a, b] = both(target => target.setVisualMesh('', null, null, null, null));
    assert.deepEqual([a, b], [{value: false}, {throws: 'TypeError'}]);
    exercised.set('setVisualMesh', 'delta');
    const legacyPositions = positions();
    const nextPositions = positions();
    assert.equal(loader.setVisualMesh('tri', legacyPositions, normals, uv, indices), true);
    assert.equal(converter.setVisualMesh('tri', nextPositions, normals, uv, indices), true);
    [a, b] = both(target => target.setCollisionMesh('collider', positions(), undefined, null,
      new Uint32Array(indices)));
    assert.deepEqual(b, a);
    note('setCollisionMesh', 'match');
    // Buffers are copied at upload time.
    legacyPositions.fill(9); nextPositions.fill(9);

    [a, b] = both(target => target.createURDFPhysicsScene(robot));
    assert.deepEqual([a, b], [{value: true}, {value: true}]);
    const legacyRobot = layer(loader.exportAsUSDA());
    const nextRobot = layer(converter.exportAsUSDA());
    // Engine-specific solver/contact attributes differ: legacy authors
    // Newton scene settings and MuJoCo/Newton mesh-collision attributes,
    // next authors mjc:timestep and mjc:contype/conaffinity.
    const engineOnly = {
      legacy: {'/World/PhysicsScene': ['newton:gravityEnabled', 'newton:maxSolverIterations',
        'newton:timeStepsPerSecond'],
      '/World/Links/base/collision': ['mjc:condim', 'mjc:inertia', 'mjc:margin', 'mjc:solmix',
        'newton:contactGap', 'newton:contactMargin', 'newton:maxHullVertices']},
      next: {'/World/PhysicsScene': ['mjc:timestep'],
        '/World/Links/base/collision': ['mjc:conaffinity', 'mjc:contype']}};
    for (const [side, table] of [['legacy', legacyRobot], ['next', nextRobot]]) {
      for (const [primPath, names] of Object.entries(engineOnly[side])) {
        for (const name of names) {
          assert.ok(name in table.prims[primPath].properties, `${side} ${primPath}.${name}`);
          delete table.prims[primPath].properties[name];
        }
      }
    }
    assert.deepEqual(nextRobot, legacyRobot, 'URDF scene apart from engine-specific attributes');
    assert.equal(nextRobot.prims['/World/Links/base/collision'].properties.purpose, '"guide"');
    exercised.set('createURDFPhysicsScene', 'delta');
    note('setVisualMesh', 'match');

    // extractPhysicsSceneJSON: next's physics JSON is its own contract (schema
    // fallbacks and geometry arrays instead of legacy's flags and matrices);
    // both describe the same prims in the same order.
    const legacyPhysics = JSON.parse(loader.extractPhysicsSceneJSON());
    const nextPhysics = JSON.parse(converter.extractPhysicsSceneJSON());
    assert.deepEqual(nextPhysics.prims.map(prim => prim.path), legacyPhysics.prims.map(prim => prim.path));
    assert.deepEqual(nextPhysics.prims.map(prim => prim.type), legacyPhysics.prims.map(prim => prim.type));
    assert.notDeepEqual(Object.keys(nextPhysics.prims[0]).sort(), Object.keys(legacyPhysics.prims[0]).sort());
    exercised.set('extractPhysicsSceneJSON', 'delta');

    [a, b] = both(target => target.clearURDFMeshBuffers());
    assert.deepEqual(b, a);
    [a, b] = both(target => target.createURDFPhysicsScene(robot));
    assert.deepEqual(b, a);
    assert.equal(layer(converter.exportAsUSDA()).prims['/World/Links/base/visual'], undefined,
      'cleared mesh buffers leave the robot without geometry');
    assert.equal(layer(loader.exportAsUSDA()).prims['/World/Links/base/visual'], undefined);
    note('clearURDFMeshBuffers', 'match');
  } finally { loader.delete(); converter.delete(); }
}

// ----------------------------------------------------------- image encoding
// Decode an encoded image through next's native texture path.
function decodePixels(bytes, extension) {
  const name = `probe.${extension}`;
  const store = new nextModule.NextAssetStore();
  const stream = new nextModule.RenderStream();
  try {
    store.setAsset(name, bytes);
    stream.setAssetStore(store);
    stream.setLoadTextureInNative(true);
    const scene = `#usda 1.0
def Material "M" {
 token outputs:surface.connect = </M/S.outputs:surface>
 def Shader "S" {
  uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor.connect = </M/T.outputs:rgb>
  token outputs:surface
 }
 def Shader "T" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @${name}@
  float3 outputs:rgb
 }
}
def Mesh "Q" (prepend apiSchemas = ["MaterialBindingAPI"]) {
 int[] faceVertexCounts = [3]
 int[] faceVertexIndices = [0,1,2]
 point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
 rel material:binding = </M>
}
`;
    assert.equal(stream.begin(encode(scene), 'probe.usda').success, true, stream.error());
    const image = stream.getImageCopy(0);
    assert.equal(image.decoded, true, `${extension} decodes`);
    return {width: image.width, height: image.height, channels: image.channels,
      pixels: Array.from(image.data)};
  } finally { stream.delete(); store.delete(); }
}
{
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    const rgba = new Uint8Array([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128]);
    const rgb = rgba.filter((_, i) => i % 4 !== 3);
    for (const [format, extension] of [['png', 'png'], ['bmp', 'bmp'], ['tiff', 'tiff'], ['exr', 'exr']]) {
      for (const [channels, pixels] of [[4, rgba], [3, rgb]]) {
        const legacyBytes = loader.encodeImageNative(pixels, 2, 2, channels, format);
        const nextBytes = stream.encodeImageNative(pixels, 2, 2, channels, format);
        assert.ok(legacyBytes instanceof Uint8Array && nextBytes instanceof Uint8Array, format);
        if (format === 'tiff') {
          // Baseline uncompressed TIFF (not a texture format the decoder
          // reads): both files must carry the raw pixel strip.
          const holds = bytes => Buffer.from(bytes).indexOf(Buffer.from(pixels)) >= 0;
          assert.ok(holds(legacyBytes) && holds(nextBytes), `${format}/${channels}: pixel strip`);
          continue;
        }
        if (format === 'exr') {
          // Both products use the same TinyEXR writer.
          assert.deepEqual(nextBytes, legacyBytes, `${format}/${channels}: identical EXR`);
          continue;
        }
        assert.deepEqual(decodePixels(nextBytes, extension), decodePixels(legacyBytes, extension),
          `${format}/${channels}: decoded pixels`);
      }
    }
    for (const format of ['jpg', 'bad'])
      assert.deepEqual(outcome(() => stream.encodeImageNative(rgba, 2, 2, 4, format)),
        outcome(() => loader.encodeImageNative(rgba, 2, 2, 4, format)), format);
    note('encodeImageNative', 'match');
  } finally { loader.delete(); stream.delete(); }
}

// ------------------------------------------------------------ bone textures
{
  const bytes = new Uint8Array(fs.readFileSync(new URL('../../../tests/usda/skelanimation-full-001.usda', import.meta.url)));
  const loader = new legacyModule.LightUSDLoaderNative();
  const stream = new nextModule.RenderStream();
  try {
    assert.equal(loader.loadFromBinary(bytes, 's.usda'), true);
    assert.equal(stream.begin(bytes, 's.usda').success, true);
    // Pair meshes by name: the converters enumerate skinned meshes differently.
    const byName = (target, mesh) => new Map(Array.from({length: target.numMeshes()},
      (_, i) => [mesh(target, i).primName, i]));
    const legacyMeshes = byName(loader, (target, i) => target.getMesh(i));
    const nextMeshes = byName(stream, (target, i) => target.getMeshCopy(i));
    assert.deepEqual([...nextMeshes.keys()].sort(), [...legacyMeshes.keys()].sort());
    const norm = texture => JSON.parse(JSON.stringify(texture,
      (key, value) => ArrayBuffer.isView(value) ? Array.from(value) : value));
    let skinned = 0;
    for (const [name, legacyId] of legacyMeshes) {
      for (const influences of [0, 2]) {
        const legacyTexture = norm(loader.generateBoneTexture(legacyId, influences));
        const nextTexture = norm(stream.generateBoneTexture(nextMeshes.get(name), influences));
        if (name === 'Body') {
          // Body binds a skeleton without applying SkelBindingAPI: legacy
          // does not skin it, next skins it leniently.
          assert.deepEqual(legacyTexture, {error: 'Mesh has no skinning data'});
          assert.ok(nextTexture.textureData?.length > 0 && nextTexture.vertexCount === 12);
          continue;
        }
        assert.deepEqual(nextTexture, legacyTexture, `${name} bone texture (${influences})`);
        if (!legacyTexture.error) ++skinned;
      }
    }
    // A properly bound mesh (SkelBindingAPI, vertex-interpolated six
    // influences) must produce identical textures at every influence cap.
    const identity = '((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))';
    const skinned6 = encode(`#usda 1.0
def SkelRoot "R" {
 def Skeleton "Skel" {
  uniform token[] joints = ["a","a/b","a/b/c","a/b/c/d","a/b/c/d/e","a/b/c/d/e/f"]
  uniform matrix4d[] bindTransforms = [${Array(6).fill(identity).join(',')}]
  uniform matrix4d[] restTransforms = [${Array(6).fill(identity).join(',')}]
 }
 def Xform "G" (prepend apiSchemas = ["SkelBindingAPI"]) {
  rel skel:skeleton = </R/Skel>
  def Mesh "M" {
   int[] faceVertexCounts = [4]
   int[] faceVertexIndices = [0,1,2,3]
   point3f[] points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]
   int[] primvars:skel:jointIndices = [${Array(4).fill('0,1,2,3,4,5').join(',')}] (interpolation = "vertex" elementSize = 6)
   float[] primvars:skel:jointWeights = [${Array(4).fill('0.3,0.25,0.2,0.1,0.1,0.05').join(',')}] (interpolation = "vertex" elementSize = 6)
  }
 }
}
`);
    const skinLoader = new legacyModule.LightUSDLoaderNative();
    const skinStream = new nextModule.RenderStream();
    try {
      assert.equal(skinLoader.loadFromBinary(skinned6, 'skin.usda'), true);
      assert.equal(skinStream.begin(skinned6, 'skin.usda').success, true);
      for (const influences of [0, 2, 4, 8]) {
        const legacyTexture = norm(skinLoader.generateBoneTexture(0, influences));
        assert.equal(legacyTexture.error, undefined);
        assert.deepEqual(norm(skinStream.generateBoneTexture(0, influences)), legacyTexture,
          `six-influence bone texture (${influences})`);
        ++skinned;
      }
    } finally { skinLoader.delete(); skinStream.delete(); }
    assert.ok(skinned > 0, 'fixture includes skinned meshes');
    assert.deepEqual(stream.generateBoneTexture(99, 0), loader.generateBoneTexture(99, 0));
    exercised.set('generateBoneTexture', 'delta');
  } finally { loader.delete(); stream.delete(); }
}

// ---------------------------------------------------------- matrix sync
const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
const claimed = matrix.filter(row => row.behaviorTest === TEST_NAME);
assert.deepEqual(claimed.map(row => row.legacyMethod).sort(), [...exercised.keys()].sort(),
  'every method exercised here must carry this behaviorTest in the parity matrix, and vice versa');
for (const row of claimed) {
  const kind = exercised.get(row.legacyMethod);
  if (row.parityStatus === 'behavior_verified') {
    assert.equal(Boolean(row.edgeDifference), kind === 'delta', row.legacyMethod);
  } else {
    assert.equal(row.parityStatus, 'known_behavior_gap', row.legacyMethod);
    assert.equal(kind, 'delta', row.legacyMethod);
  }
}
const gaps = claimed.filter(row => row.parityStatus === 'known_behavior_gap').length;
console.log(`ok - schema/image utility behavior parity (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${claimed.length - gaps} methods verified against legacy, ${gaps} documented gaps`);
