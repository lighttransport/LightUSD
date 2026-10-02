import test from 'node:test';
import assert from 'node:assert/strict';
import { LuciaUsdSession } from '../src/usd-session.js';
import { scanUSDAssetReferences } from '../src/usd-dependencies.js';
import { diagnoseUSD } from '../src/usd-doctor.js';
import { analyzeAsset } from '../src/asset-report.js';
import { snapshotHealthInput } from '../src/health-analysis.js';

test('typed property flags preserve schema and custom attributes on WASM32 and WASM64', async () => {
  for (const suffix of ['', '_64']) {
    const { default: factory } = await import(`../../src/lightusd/lightusd_next${suffix}.js`);
    const module = await factory(), document = new module.LayerDocument();
    try {
      assert.equal(document.load(new TextEncoder().encode('#usda 1.0\ndef Xform "P" {}')).success, true);
      assert.equal(document.setAttribute('/P', 'xformOpOrder', 'token', ['xformOp:translate'], true,
        { uniform: true, custom: false }).success, true);
      assert.equal(document.setAttribute('/P', 'visibility', 'token', 'invisible', false, { custom: false }).success, true);
      assert.equal(document.setAttribute('/P', 'user:test', 'double', 1.123456789).success, true);
      const properties = JSON.parse(document.exportJSON().text).primSpecs.P.properties;
      assert.equal(properties.xformOpOrder.isCustom, false);
      assert.equal(properties.xformOpOrder.attribute.variability, 'uniform');
      assert.equal(properties.visibility.isCustom, false);
      assert.equal(properties['user:test'].isCustom, true);
      assert.throws(() => document.setAttribute('/P', 'bad', 'token', 'x', false, { uniform: 1 }));
    } finally { document.delete(); }
  }
});

test('unindexed face-varying primvars retain corner identity', () => {
  const session = new LuciaUsdSession();
  session.usda = `#usda 1.0
def Mesh "M" {
 int[] faceVertexIndices = [0, 1, 2]
 texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] ( interpolation = "faceVarying" )
 color3f[] primvars:corner = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] ( interpolation = "faceVarying" )
}`;
  assert.deepEqual([...session.getMeshUVData('/M').uvIndices], [0, 1, 2]);
  assert.deepEqual([...session.getMeshFaceVaryingPrimvars('/M', 3)[0].indices], [0, 1, 2]);
  assert.throws(() => session.getMeshFaceVaryingPrimvars('/M', 6), /malformed/);
  session.usda = session.usda.replace('int[] faceVertexIndices = [0, 1, 2]', 'int[] faceVertexIndices = [0, 1, 2]\nint[] primvars:st:indices = [-1, 1, 2]');
  assert.throws(() => session.getMeshUVData('/M'), /malformed/);
});
test('face-varying extraction rejects numeric narrowing instead of changing authored data', () => {
  const session = new LuciaUsdSession();
  for (const [type, value] of [['int', '1.5'], ['int', '2147483648'], ['float', '1e100']]) {
    session.usda = `#usda 1.0\ndef Mesh "M" { ${type}[] primvars:corner = [${value}, 0, 1] ( interpolation = "faceVarying" ) }`;
    assert.throws(() => session.getMeshFaceVaryingPrimvars('/M', 3), { code: 'LUCIA_CLEANUP_FACEVARYING' });
  }
});
test('dependency kinds follow assignments, including nested arc options', () => {
  assert.deepEqual(scanUSDAssetReferences('asset file = @@@textures/contact@2x.png@@@'),
    [{ path: 'textures/contact@2x.png', kind: 'asset' }]);
  const references = scanUSDAssetReferences(`#usda 1.0
(subLayers = [@layers/a.usda@])
def Xform "P" (references = [@one.usda@ (offset = 2), @two.usda@]) {
 string note = "payload @fake.usda@"
 asset inputs:file = @layers/a.usda@
 # references = @ignored.usda@
}`);
  assert.deepEqual(references, [{ path: 'layers/a.usda', kind: 'sublayers' },
    { path: 'one.usda', kind: 'references' }, { path: 'two.usda', kind: 'references' },
    { path: 'layers/a.usda', kind: 'asset' }]);
});
test('nested layers resolve relative paths; asset references are not composition cycles', () => {
  const bytes = source => ({ bytes: new TextEncoder().encode(source) });
  const assets = new Map([['layers/a.usda', bytes('#usda 1.0\n(subLayers = [@b.usda@])')],
    ['layers/b.usda', bytes('#usda 1.0\ndef Xform "B" { asset custom:file = @a.usda@ }')]]);
  const report = diagnoseUSD('#usda 1.0\n(subLayers = [@layers/a.usda@])', assets);
  assert.ok(report.dependencies.some(item => item.path === 'layers/b.usda' && item.present));
  assert.ok(!report.issues.some(item => item.ruleId === 'usd.compositionCycle'));
  assets.set('layers/b.usda', bytes('#usda 1.0\n(subLayers = [@a.usda@])'));
  assert.ok(diagnoseUSD('#usda 1.0\n(subLayers = [@layers/a.usda@])', assets).issues.some(item => /cycle/i.test(item.ruleId)));
});
test('worker Health input preserves diagnostics without detaching render buffers', () => {
  const position = { array: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), count: 3, itemSize: 3 };
  const node = { isMesh: true, name: '/M', geometry: { attributes: { position }, groups: [] },
    material: { uuid: 'material', type: 'MeshStandardMaterial', roughness: .5, color: { isColor: true, getHexString: () => 'ffffff' } } };
  const root = { traverse: visit => visit(node) };
  const { nodes } = snapshotHealthInput(root);
  assert.deepEqual(analyzeAsset({ traverse: visit => nodes.forEach(visit) }), analyzeAsset(root));
  assert.equal(position.array.byteLength, 36);
  assert.throws(() => snapshotHealthInput(root, 1), { code: 'LUCIA_ANALYSIS_MEMORY' });
});

test('binary dependency inspection is explicit and a decoded host view closes it', () => {
  const assets = new Map([['binary.usdc', { bytes: Uint8Array.of(80, 88, 82) }]]);
  const source = '#usda 1.0\n(subLayers = [@binary.usdc@])';
  const report = diagnoseUSD(source, assets);
  assert.equal(report.issues.filter(item => item.ruleId === 'usd.uninspectedLayer').length, 1);
  const decoded = diagnoseUSD(source, assets, { layerSources: new Map([['binary.usdc', '#usda 1.0\n(subLayers = [@missing.usda@])']]) });
  assert.ok(decoded.dependencies.some(item => item.path === 'missing.usda' && !item.present));
  assert.ok(!decoded.issues.some(item => item.ruleId === 'usd.uninspectedLayer'));
});

test('typed native transforms preserve double precision and roll back a failed batch', async () => {
  const session = new LuciaUsdSession();
  await session.init();
  try {
    await session.loadUSDA('#usda 1.0\ndef Xform "P" {}\n');
    await session.setTransform('/P', { translate: [1.123456789012345, 0, 0], rotate: [0, 0, 0], scale: [1, 1, 1] });
    assert.match(session.usda, /1\.123456789/);
    await session.setVisibility('/P', false);
    assert.match(session.usda, /visibility = "invisible"/);
    const before = session.usda;
    await assert.rejects(() => session.nativeAttributeEdits([
      { tool: 'attr_set', args: { path: '/P', attr_name: 'visibility', value: { type: 'token', value: 'inherited' } } },
      { tool: 'attr_set', args: { path: '/Missing', attr_name: 'visibility', value: { type: 'token', value: 'invisible' } } },
    ], 'Injected failed batch'));
    assert.equal(session.usda, before);
    assert.equal(session.call('stage_to_string').usda, before);
  } finally { session.dispose(); }
});

test('typed mesh edits keep indexed corner buffers and four-component tangents', async () => {
  const session = new LuciaUsdSession();
  session.replaceUSDA = async function(source) { const previous = this.usda; this.usda = source; return previous; };
  session.usda = '#usda 1.0\ndef Mesh "M" { custom string note = "keep" }';
  await session.setMeshGeometry('/M', {
    positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]), indices: new Uint32Array([0, 1, 2]),
    uvs: new Float32Array([0, 0, 1, 0]), uvIndices: new Uint32Array([0, 1, 0]),
    tangents: new Float32Array([1, 0, 0, 1, 1, 0, 0, -1, 1, 0, 0, 1]),
    customAttributes: [{ name: 'precise', itemSize: 1, array: new Float64Array([1.1234567890123, 2, 3]) }],
  });
  assert.match(session.usda, /float4\[\] primvars:tangents/);
  assert.match(session.usda, /double\[\] primvars:precise/);
  assert.match(session.usda, /1\.1234567890123/);
  const uv = session.getMeshUVData('/M');
  assert.deepEqual([...uv.uvIndices], [0, 1, 0]);
  assert.equal(uv.uvs.length, 4);
  assert.match(session.usda, /custom string note = "keep"/);
  const before = session.usda;
  await assert.rejects(() => session.nativeAttributeEdits([{ args: { path: '/M', attr_name: 'points',
    value: { type: 'point3f[]', value: [0, 0, 0] }, metadata: { interpolation: 'vertex' } } },
    { args: { path: '/Missing', attr_name: 'points', value: { type: 'point3f[]', value: [0, 0, 0] } } }], 'failed'));
  assert.equal(session.usda, before);
});
