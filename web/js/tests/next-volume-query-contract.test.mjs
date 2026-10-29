// SPDX-License-Identifier: Apache-2.0
// UsdVol render query contract: sparse/dense voxel queries and copied Gaussian
// particle arrays exposed by RenderStream. The VDB payload path is covered by
// next_test_volume_c and the Python suite; this pins JS argument validation,
// unresolved-field behavior, and ownership of returned particle arrays.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: createNext} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const Module = await createNext();

for (const name of ['getVolumes', 'getParticleField', 'getVolumeDense',
                    'getVolumeNanovdb', 'getVolumeSample']) {
  assert.equal(typeof Module.RenderStream.prototype[name], 'function', `${name} is exported`);
}

const usda = `#usda 1.0
def Volume "V"
{
    rel field:density = </V/density>
    def OpenVDBAsset "density"
    {
        asset filePath = @missing.vdb@
        token fieldName = "density"
    }
}
`;
const stream = new Module.RenderStream();
try {
  const begun = stream.begin(new TextEncoder().encode(usda), 'volume-contract.usda');
  assert.equal(begun.success, true, `${stream.error()}`);

  const volumes = stream.getVolumes();
  assert.equal(volumes.volumes.length, 1, 'the Volume record is kept');
  assert.equal(volumes.volumes[0].primPath, '/V');
  // The referenced .vdb does not resolve, so the field is skipped and the
  // queries below must return null rather than throwing.
  assert.equal(volumes.volumes[0].fields.length, 0);

  assert.equal(stream.getVolumeNanovdb(0, 0), null);
  assert.equal(stream.getVolumeSample(0, 0, [[0, 0, 0]]), null);
  assert.equal(stream.getVolumeSample(0, 0, [0, 0, 0, 1, 1, 1]), null);

  for (const [label, call] of [
    ['fractional volume', () => stream.getVolumeNanovdb(0.5, 0)],
    ['fractional field', () => stream.getVolumeSample(0, 0.5, [])],
    ['odd flat points', () => stream.getVolumeSample(0, 0, [1, 2])],
    ['short triplet', () => stream.getVolumeSample(0, 0, [[1, 2]])],
  ]) {
    assert.throws(call, TypeError, label);
  }

  // Out-of-range indices are not an error at the JS layer: the native query
  // reports "no such resource" and the wrapper returns null.
  assert.equal(stream.getVolumeNanovdb(9, 9), null);
  assert.equal(stream.getVolumeSample(9, 9, []), null);
} finally {
  stream.delete();
}
console.log('ok - volume NanoVDB/sample query contract');

// Gaussian particle arrays are copied out of the WASM heap and include the
// schema defaults/materialized coefficient layout expected by CPU clients.
{
  const particleStream = new Module.RenderStream();
  try {
    const particlePath = new URL('../../../tests/usda/lusdview-gaussian-splat.usda', import.meta.url);
    const loaded = particleStream.begin(new Uint8Array(readFileSync(particlePath)), 'particles.usda');
    assert.equal(loaded.success, true, particleStream.error());
    const catalog = particleStream.getVolumes();
    assert.equal(catalog.volumes.length, 0);
    assert.equal(catalog.particleFields.length, 1);
    assert.equal(catalog.particleFields[0].primPath, '/GaussianFixture/SplatField');
    assert.equal(catalog.particleFields[0].count, 2);
    assert.equal(catalog.particleFields[0].shDegree, 0);

    const field = particleStream.getParticleField(0);
    assert.ok(field.positions instanceof Float32Array);
    assert.ok(field.orientations instanceof Float32Array);
    assert.ok(field.scales instanceof Float32Array);
    assert.ok(field.opacities instanceof Float32Array);
    assert.ok(field.shCoefficients instanceof Float32Array);
    assert.deepEqual(field.positions, new Float32Array([-0.35, 0, 0, 0.35, 0, 0]));
    assert.deepEqual(field.scales, new Float32Array([0.18, 0.08, 0.02, 0.08, 0.18, 0.02]));
    assert.deepEqual(field.opacities, new Float32Array([0.9, 0.75]));
    assert.deepEqual(field.shCoefficients, new Float32Array([1, 0.2, 0.05, 0.05, 0.3, 1]));
    assert.deepEqual(field.orientations, new Float32Array([1, 0, 0, 0, 1, 0, 0, 0]));
    assert.equal(particleStream.getParticleField(9), null);
    const secondCopy = particleStream.getParticleField(0).positions;
    assert.notEqual(field.positions.buffer, secondCopy.buffer);
    secondCopy[0] = 99;
    assert.deepEqual(field.positions, new Float32Array([-0.35, 0, 0, 0.35, 0, 0]));
  } finally {
    particleStream.delete();
  }
}
console.log('ok - Gaussian particle field copied-array query contract');

// Exercise real VDB bytes when the same optional corpus used by the native
// volume tests is available. Keep the missing-field contract runnable without it.
if (process.env.TINYVDBIO_DATA_DIR) {
  const store = new Module.NextAssetStore();
  const realStream = new Module.RenderStream();
  try {
    store.setAsset('root.usda', new TextEncoder().encode(`#usda 1.0
def Volume "V" {
  rel field:density = </V/density>
  def OpenVDBAsset "density" {
    asset filePath = @field.vdb@
    token fieldName = "float_test"
  }
}`));
    store.setAsset('field.vdb', new Uint8Array(readFileSync(join(
      process.env.TINYVDBIO_DATA_DIR, 'reference', 'ref_float.vdb'))));
    const loaded = realStream.beginCachedAsset(store, 'root.usda');
    assert.equal(loaded.success, true, realStream.error());
    const volumes = realStream.getVolumes().volumes;
    assert.equal(volumes.length, 1);
    assert.equal(volumes[0].fields.length, 1);
    const dense = realStream.getVolumeDense(0, 0);
    assert.deepEqual(dense.dims, [4, 4, 4]);
    assert.equal(dense.data.length, 64);
    const nano = realStream.getVolumeNanovdb(0, 0);
    assert.equal(nano.components, 1);
    assert.match(new TextDecoder().decode(nano.data.slice(0, 8)), /^NanoVDB[01]$/);
    const sample = realStream.getVolumeSample(0, 0, [[0.5, 0.5, 0.5]]);
    assert.equal(sample.count, 1);
    assert.equal(sample.components, 1);
    assert.ok(Number.isFinite(sample.data[0]));
    assert.ok(sample.data[0] >= Math.min(...dense.data) &&
              sample.data[0] <= Math.max(...dense.data));
    console.log('ok - volume queries with reference VDB');
  } finally {
    realStream.delete();
    store.delete();
  }
} else {
  console.log('skip - set TINYVDBIO_DATA_DIR for reference VDB queries');
}
