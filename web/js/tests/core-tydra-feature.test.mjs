// SPDX-License-Identifier: Apache-2.0
// The standalone Tydra feature must load as an independent WASM module, own
// its own C++ objects, and convert USD bytes copied in from the core heap.
import assert from 'node:assert/strict';

const suffix = process.env.LIGHTUSD_WASM64 === '1' ? '_64' : '';
const {default: factory} = await import(`../src/lightusd/lightusd${suffix}.js`);
const core = await factory();
const pending = core.loadFeature('tydra');
assert.equal(core.loadFeature('tydra'), pending, 'feature promise is deduplicated');
const tydra = await pending;
assert.equal(core.loadFeature('tydra'), pending, 'resolved feature is shared');

// Independent heaps are the precondition for the copy-in bridge; a module that
// shares the core heap would silently alias C++ objects across two runtimes.
assert.notEqual(tydra.HEAPU8.buffer, core.HEAPU8.buffer, 'independent heap');
assert.deepEqual(tydra.featureInfo, core.featureInfo, 'matching ABI metadata');
assert.equal(typeof tydra.convertToRenderScene, 'function', 'conversion bridge');

const encoder = new TextEncoder();
const usda = `#usda 1.0
def Xform "World" {
  def Mesh "Quad" {
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]
  }
}
`;
const bytes = encoder.encode(usda);
const result = tydra.convertToRenderScene(bytes);
assert.equal(result.ok, true, result.error);
assert.equal(result.meshes, 1);
assert.ok(result.nodes >= 1, 'scene has a node table');
assert.equal(result.firstMesh.pointCount, 4);
assert.equal(result.firstMesh.indexCount, 4);
assert.match(result.firstMesh.primPath, /Quad$/);

// Views backed by the caller's heap must be copied, not aliased: freeing the
// core allocation afterwards cannot affect the feature's copy.
const ptr = core._lightusd_wasm_alloc(bytes.length);
assert.ok(ptr);
try {
  core.HEAPU8.set(bytes, Number(ptr));
  const heapView = core.HEAPU8.subarray(Number(ptr), Number(ptr) + bytes.length);
  const fromHeap = tydra.convertToRenderScene(heapView);
  assert.equal(fromHeap.ok, true, fromHeap.error);
  assert.deepEqual(fromHeap.firstMesh, result.firstMesh);
} finally {
  core._lightusd_wasm_free(ptr);
}

// Malformed input is a returned error, not a throw or a crash.
const bad = tydra.convertToRenderScene(encoder.encode('not a USD file'));
assert.equal(bad.ok, false);
assert.equal(typeof bad.error, 'string');
assert.ok(bad.error.length > 0);

for (const badArg of [null, 5, {}, [1, 2, 3]])
  assert.throws(() => tydra.convertToRenderScene(badArg), /expected byte view/);
assert.throws(() => tydra.convertToRenderScene(new Uint8Array(4), {maxMemoryMB: 1}),
  /maxMemoryMB out of range/);

// The feature owns a stateful RenderStream too: no C++ object is shared with
// the core module, and its methods operate entirely on the feature's heap.
assert.equal(typeof tydra.RenderStream, 'function');
const stream = new tydra.RenderStream();
assert.equal(stream.meshCount(), 0);
const began = stream.begin(bytes);
assert.equal(began.success, true, began.error);
assert.equal(stream.meshCount(), 1);
assert.equal(stream.nodeCount(), result.nodes);
assert.equal(stream.numMaterials(), result.materials);
assert.equal(stream.getMesh(0).points.length / 3, 4);
stream.end();
assert.equal(stream.meshCount(), 0);
assert.equal(stream.begin(encoder.encode('not USD')).success, false);
stream.delete();
assert.equal(stream.isDeleted(), true);
assert.throws(() => stream.meshCount(), /Invalid LightUSD receiver/);
assert.throws(() => stream.delete(), /already deleted/);

console.log('ok - standalone Tydra feature: independent heap, copy-in conversion and owned RenderStream');
