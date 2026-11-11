// Compare legacy layerToRenderScene with the split core LayerDocument/RenderStream workflow.
import assert from 'node:assert/strict';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const legacyDefault = wasm64 ? '../src/lightusd/lightusd_legacy_64.js'
                               : '../src/lightusd/lightusd_legacy.js';
const legacyUrl = process.env.LIGHTUSD_LEGACY_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_LEGACY_MODULE)).href
  : new URL(legacyDefault, import.meta.url).href;
const legacyModule = await loadWasm(() => import(legacyUrl));
const coreGlue = wasm64 ? '../src/lightusd/lightusd_64.js'
                        : '../src/lightusd/lightusd.js';
const {default: createCoreModule} = await import(new URL(coreGlue, import.meta.url));
const coreModule = await createCoreModule();
const legacy = new legacyModule.LightUSDLoaderNative();
const document = new coreModule.LayerDocument();
const stream = new coreModule.RenderStream();
const bytes = new TextEncoder().encode(`#usda 1.0

def Xform "Root" {
    def Mesh "Triangle" {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
}
`);
try {
  assert.equal(legacy.loadAsLayerFromBinary(bytes, 'layer-render.usda'), true, legacy.error());
  assert.equal(legacy.layerToRenderScene(), true, legacy.error());
  const expected = legacy.getMemoryStats();
  assert.equal(document.load(bytes).success, true, document.error());
  const actual = stream.beginFromLayerDocument(document);
  assert.equal(actual.success, true, actual.error || stream.error());
  const coreStats = stream.getStats();
  assert.equal(coreStats.renderSceneNodes, expected.numNodes + 1,
    'core currently includes its explicit scene root in node counts');
  assert.equal(coreStats.renderSceneMeshes, expected.numMeshes);
  assert.equal(coreStats.renderSceneMaterials, expected.numMaterials);
  assert.equal(coreStats.renderSceneTextures, expected.numTextures);
  assert.equal(coreStats.renderSceneImages, expected.numImages);
  assert.equal(coreStats.renderSceneLights, expected.numLights);
  assert.equal(expected.numMeshes, 1, 'fixture must exercise a mesh conversion');
} finally {
  stream.delete();
  document.delete();
  legacy.delete();
}
console.log(`ok - layerToRenderScene vs LayerDocument/RenderStream parity (${wasm64 ? 'wasm64' : 'wasm32'})`);
