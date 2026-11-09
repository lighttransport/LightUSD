// Compare next-only material JSON/XML serialization with the legacy loader.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const combinedDefault = wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                               : '../src/lightusd/lightusd_combined.js';
const combinedUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(combinedDefault, import.meta.url).href;
const legacyModule = await loadWasm(() => import(combinedUrl));
const nextGlue = wasm64 ? '../src/lightusd/lightusd_next_64.js'
                        : '../src/lightusd/lightusd_next.js';
const {default: createNextModule} = await import(new URL(nextGlue, import.meta.url));
const nextModule = await createNextModule();

for (const fixture of [
  '../../../tests/usda/next-material-format-parity.usda',
  '../../../tests/usda/c-core-material-queries.usda'
]) {
  const bytes = new Uint8Array(fs.readFileSync(new URL(fixture, import.meta.url)));
  const legacy = new legacyModule.LightUSDLoaderNative();
  const next = new nextModule.RenderStream();
  try {
    const legacyLoaded = legacy.loadFromBinary(bytes, path.basename(fixture));
    assert.equal(legacyLoaded, true, legacy.error());
    const load = next.begin(bytes);
    assert.equal(load.success, true, load.error || next.error());
    const nextMaterial = next.getAllMaterials()[0];
    assert.ok(nextMaterial, `next material is missing for ${fixture}`);

    if (fixture.includes('c-core-material-queries')) {
      const expected = legacy.getMaterialWithFormat(0, 'legacy');
      assert.equal(expected.useSpecularWorkflow, true);
      assert.equal(expected.materialXConfig.authored, true);
      assert.equal(typeof expected.diffuseColorTextureId, 'number');
      assert.equal(typeof expected.roughnessTextureId, 'number');
    }

    for (const format of ['json', 'xml', 'legacy', '']) {
      const oldResult = legacy.getMaterialWithFormat(0, format);
      const nextResult = next.getMaterialWithFormat(nextMaterial.id, format);
      assert.deepEqual(nextResult, oldResult,
        `${format} material serialization differs for ${fixture}`);
    }
    assert.deepEqual(next.getMaterialWithFormat(-1, 'legacy'),
      legacy.getMaterialWithFormat(-1, 'legacy'));
    assert.deepEqual(next.getMaterialWithFormat(nextMaterial.id, 'bad'),
      legacy.getMaterialWithFormat(0, 'bad'));
  } finally {
    legacy.delete();
    next.delete();
  }
}

// Exercise the metallic branch separately: the legacy object omits
// specularColor when useSpecularWorkflow is false.
{
  const bytes = new TextEncoder().encode(`#usda 1.0
def Xform "World" {
 def Mesh "Surface" {
  int[] faceVertexCounts = [3]
  int[] faceVertexIndices = [0, 1, 2]
  point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
  rel material:binding = </World/Material>
 }
 def Material "Material" {
  token outputs:surface.connect = </World/Material/Shader.outputs:surface>
  def Shader "Shader" {
   uniform token info:id = "UsdPreviewSurface"
   int inputs:useSpecularWorkflow = 0
   float inputs:metallic = 0.75
   token outputs:surface
  }
 }
}`);
  const legacy = new legacyModule.LightUSDLoaderNative();
  const next = new nextModule.RenderStream();
  try {
    assert.equal(legacy.loadFromBinary(bytes, 'metallic.usda'), true, legacy.error());
    const load = next.begin(bytes);
    assert.equal(load.success, true, load.error || next.error());
    const material = next.getAllMaterials()[0];
    assert.ok(material);
    const expected = legacy.getMaterialWithFormat(0, 'legacy');
    assert.equal(expected.metallic, 0.75);
    assert.equal(Object.hasOwn(expected, 'specularColor'), false);
    assert.deepEqual(next.getMaterialWithFormat(material.id, 'legacy'), expected);
  } finally {
    legacy.delete();
    next.delete();
  }
}

// Light serialization is its own legacy API family. Keep a type-rich fixture
// as the oracle for common, type-specific light properties and unsupported XML.
{
  const fixture = '../../../tests/usda/lights-full-001.usda';
  const bytes = new Uint8Array(fs.readFileSync(new URL(fixture, import.meta.url)));
  const legacy = new legacyModule.LightUSDLoaderNative();
  const next = new nextModule.RenderStream();
  try {
    assert.equal(legacy.loadFromBinary(bytes, path.basename(fixture)), true, legacy.error());
    const load = next.begin(bytes);
    assert.equal(load.success, true, load.error || next.error());
    const nextLights = next.getAllLights();
    const nextLightIds = new Map(nextLights.map(light => [light.primPath, light.index]));
    const legacyLights = legacy.getAllLights();
    assert.ok(legacyLights.length > 0);
    let serializedLights = 0;
    for (let legacyId = 0; legacyId < legacyLights.length; ++legacyId) {
      const legacyLight = legacyLights[legacyId];
      const lightId = nextLightIds.get(legacyLight.absPath);
      assert.notEqual(lightId, undefined,
        `next render scene is missing legacy light ${legacyLight.absPath}`);
      for (const format of ['json', 'xml']) {
        assert.deepEqual(next.getLightWithFormat(lightId, format),
          legacy.getLightWithFormat(legacyId, format),
          `${format} light serialization differs for ${legacyLight.absPath}`);
      }
      ++serializedLights;
    }
    assert.equal(serializedLights, legacyLights.length,
      'fixture should cover every legacy light, including time-sampled lights');
    assert.deepEqual(next.getLightWithFormat(-1, 'bad'),
      {error: 'Invalid light ID'});
    assert.deepEqual(next.getLightWithFormat(0, 'bad'),
      {error: "Unsupported format. Use 'json' or 'xml'"});
    assert.deepEqual(next.getLightWithFormat(0, new TextEncoder().encode('json')),
      legacy.getLightWithFormat(0, new TextEncoder().encode('json')));
    assert.deepEqual(next.getLightWithFormat(0, 'json\0'),
      {error: "Unsupported format. Use 'json' or 'xml'"});
    assert.throws(() => next.getLightWithFormat(0, {}), TypeError);
  } finally {
    legacy.delete();
    next.delete();
  }
}

const matrix = JSON.parse(fs.readFileSync(new URL('./next-wasm-parity-gaps.json', import.meta.url)));
assert.deepEqual(matrix.filter(row => row.behaviorTest === 'material-format-parity.test.mjs')
  .map(row => [row.legacyMethod, row.parityStatus]), [
  ['getLightWithFormat', 'behavior_verified'], ['getMaterial', 'behavior_verified'],
  ['getMaterialWithFormat', 'behavior_verified'],
], 'material/light serialization rows must record this paired test');
console.log(`ok - next material and light formats match legacy serialization (${wasm64 ? 'wasm64' : 'wasm32'})`);
