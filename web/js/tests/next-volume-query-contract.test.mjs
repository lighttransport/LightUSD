// SPDX-License-Identifier: Apache-2.0
// UsdVol render query contract: the NanoVDB export and world-space sampling
// methods exposed by RenderStream. The voxel data path is covered end to end by
// the C API test (next_test_volume_c) and the Python suite; this pins the JS
// surface: method presence, argument validation, the null results for fields
// that do not resolve, and that a failed field never throws.
import assert from 'node:assert/strict';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: createNext} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const Module = await createNext();

for (const name of ['getVolumeNanovdb', 'getVolumeSample']) {
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
