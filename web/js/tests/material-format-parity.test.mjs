// Compare core-only material JSON/XML serialization with the legacy loader.
import assert from 'node:assert/strict';
import fs from 'node:fs';
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
const nextGlue = wasm64 ? '../src/lightusd/lightusd_64.js'
                        : '../src/lightusd/lightusd.js';
const {default: createCoreModule} = await import(new URL(nextGlue, import.meta.url));
const coreModule = await createCoreModule();

for (const fixture of [
  '../../../tests/usda/core-material-format-parity.usda',
  '../../../tests/usda/c-core-material-queries.usda'
]) {
  const bytes = new Uint8Array(fs.readFileSync(new URL(fixture, import.meta.url)));
  const legacy = new legacyModule.LightUSDLoaderNative();
  const core = new coreModule.RenderStream();
  try {
    const legacyLoaded = legacy.loadFromBinary(bytes, path.basename(fixture));
    assert.equal(legacyLoaded, true, legacy.error());
    const load = core.begin(bytes);
    assert.equal(load.success, true, load.error || core.error());
    const nextMaterial = core.getAllMaterials()[0];
    assert.ok(nextMaterial, `core material is missing for ${fixture}`);

    if (fixture.includes('c-core-material-queries')) {
      const expected = legacy.getMaterialWithFormat(0, 'legacy');
      assert.equal(expected.useSpecularWorkflow, true);
      assert.equal(expected.materialXConfig.authored, true);
      assert.equal(typeof expected.diffuseColorTextureId, 'number');
      assert.equal(typeof expected.roughnessTextureId, 'number');
    }

    for (const format of ['json', 'xml', 'legacy', '']) {
      const oldResult = legacy.getMaterialWithFormat(0, format);
      const nextResult = core.getMaterialWithFormat(nextMaterial.id, format);
      assert.deepEqual(nextResult, oldResult,
        `${format} material serialization differs for ${fixture}`);
    }
    assert.deepEqual(core.getMaterialWithFormat(-1, 'legacy'),
      legacy.getMaterialWithFormat(-1, 'legacy'));
    assert.deepEqual(core.getMaterialWithFormat(nextMaterial.id, 'bad'),
      legacy.getMaterialWithFormat(0, 'bad'));
  } finally {
    legacy.delete();
    core.delete();
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
  const core = new coreModule.RenderStream();
  try {
    assert.equal(legacy.loadFromBinary(bytes, 'metallic.usda'), true, legacy.error());
    const load = core.begin(bytes);
    assert.equal(load.success, true, load.error || core.error());
    const material = core.getAllMaterials()[0];
    assert.ok(material);
    const expected = legacy.getMaterialWithFormat(0, 'legacy');
    assert.equal(expected.metallic, 0.75);
    assert.equal(Object.hasOwn(expected, 'specularColor'), false);
    assert.deepEqual(core.getMaterialWithFormat(material.id, 'legacy'), expected);
  } finally {
    legacy.delete();
    core.delete();
  }
}

// Light serialization is its own legacy API family. Keep a type-rich fixture
// as the oracle for common, type-specific light properties and unsupported XML.
{
  const fixture = '../../../tests/usda/lights-full-001.usda';
  const bytes = new Uint8Array(fs.readFileSync(new URL(fixture, import.meta.url)));
  const legacy = new legacyModule.LightUSDLoaderNative();
  const core = new coreModule.RenderStream();
  try {
    assert.equal(legacy.loadFromBinary(bytes, path.basename(fixture)), true, legacy.error());
    const load = core.begin(bytes);
    assert.equal(load.success, true, load.error || core.error());
    const nextLights = core.getAllLights();
    const nextLightIds = new Map(nextLights.map(light => [light.primPath, light.index]));
    const legacyLights = legacy.getAllLights();
    assert.ok(legacyLights.length > 0);
    let serializedLights = 0;
    for (let legacyId = 0; legacyId < legacyLights.length; ++legacyId) {
      const legacyLight = legacyLights[legacyId];
      const lightId = nextLightIds.get(legacyLight.absPath);
      assert.notEqual(lightId, undefined,
        `core render scene is missing legacy light ${legacyLight.absPath}`);
      for (const format of ['json', 'xml']) {
        assert.deepEqual(core.getLightWithFormat(lightId, format),
          legacy.getLightWithFormat(legacyId, format),
          `${format} light serialization differs for ${legacyLight.absPath}`);
      }
      ++serializedLights;
    }
    assert.equal(serializedLights, legacyLights.length,
      'fixture should cover every legacy light, including time-sampled lights');
    assert.deepEqual(core.getLightWithFormat(-1, 'bad'),
      {error: 'Invalid light ID'});
    assert.deepEqual(core.getLightWithFormat(0, 'bad'),
      {error: "Unsupported format. Use 'json' or 'xml'"});
    assert.deepEqual(core.getLightWithFormat(0, new TextEncoder().encode('json')),
      legacy.getLightWithFormat(0, new TextEncoder().encode('json')));
    assert.deepEqual(core.getLightWithFormat(0, 'json\0'),
      {error: "Unsupported format. Use 'json' or 'xml'"});
    assert.throws(() => core.getLightWithFormat(0, {}), TypeError);
  } finally {
    legacy.delete();
    core.delete();
  }
}

const matrix = JSON.parse(fs.readFileSync(new URL('./core-wasm-parity-gaps.json', import.meta.url)));
assert.deepEqual(matrix.filter(row => row.behaviorTest === 'material-format-parity.test.mjs')
  .map(row => [row.legacyMethod, row.parityStatus]), [
  ['getLightWithFormat', 'behavior_verified'], ['getMaterial', 'behavior_verified'],
  ['getMaterialWithFormat', 'behavior_verified'],
], 'material/light serialization rows must record this paired test');
console.log(`ok - core material and light formats match legacy serialization (${wasm64 ? 'wasm64' : 'wasm32'})`);
