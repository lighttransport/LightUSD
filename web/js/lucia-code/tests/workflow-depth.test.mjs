import test from 'node:test';
import assert from 'node:assert/strict';
import { composeProject } from '../src/composition.js';
import { localizeProjectDependencies } from '../src/dependency-localization.js';
import { LuciaUsdSession, findPrimBlock } from '../src/usd-session.js';
import { LuciaCommandStack, sessionCommand } from '../src/command-stack.js';
import { workerPayloadBytes, monitorWorker } from '../src/worker-policy.js';
import { analyzeRigPoseSequence } from '../src/rig-analysis.js';
import { evaluateTargetProfile, createQualityGate, validateQualityGate } from '../src/target-profiles.js';
const encoder = new TextEncoder();
if (!globalThis.CustomEvent) globalThis.CustomEvent = class extends Event { constructor(type, options) { super(type); this.detail = options?.detail; } };
const fakeSession = source => { const session = new LuciaUsdSession(); session.usda = source;
  session.replaceUSDA = async function(text) { const old = this.usda; this.usda = text; return old; };
  return session; };
test('composition loads nested defaultPrim arcs and host resolver assets', async () => {
  const assets = new Map([['layers/a.usda', { bytes: encoder.encode('#usda 1.0\n(defaultPrim = "A")\ndef Xform "A" (references = @b.usda@) {}') }],
    ['layers/b.usda', { bytes: encoder.encode('#usda 1.0\n(defaultPrim = "B")\ndef Xform "B" { int marker = 42 }') }]]);
  const text = await composeProject('#usda 1.0\ndef Xform "P" (references = @layers/a.usda@) {}', assets);
  assert.match(text, /int marker = 42/);
  await assert.rejects(() => composeProject('#usda 1.0\ndef Xform "P" (references = @missing.usda@) {}', assets), { code: 'LUCIA_COMPOSITION_ASSET' });
  const host = await composeProject('#usda 1.0\ndef Xform "P" (references = @outside.usda@) {}', new Map(),
    { resolveAsset: async () => assets.get('layers/b.usda').bytes });
  assert.match(host, /marker = 42/);
  const abort = new AbortController(); abort.abort();
  await assert.rejects(() => composeProject(text, assets, { signal: abort.signal }), { code: 'LUCIA_CANCELLED' });
});
test('localization adjusts nested paths when moving the layer and texture', async () => {
  const assets = new Map([['layers/a.usda', { bytes: encoder.encode('#usda 1.0\ndef Scope "A" { asset texture = @../textures/a.png@ string note = "@fake.png@" }') }],
    ['textures/a.png', { bytes: Uint8Array.of(1) }]]);
  const result = await localizeProjectDependencies('#usda 1.0\n(subLayers = [@layers/a.usda@])', assets,
    { 'layers/a.usda': 'a.usda', 'textures/a.png': 'images/a.png' });
  assert.match(result.source, /@a.usda@/);
  const nested = new TextDecoder().decode(result.assets.get('a.usda').bytes);
  assert.match(nested, /@images\/a.png@/); assert.match(nested, /"@fake.png@"/);
  assert.ok(assets.has('layers/a.usda')); assert.ok(!result.assets.has('layers/a.usda'));
});
test('localization rewrites resolver-fetched layers and rejects destination collisions', async () => {
  const source = '#usda 1.0\n(subLayers = [@https://assets.invalid/layers/a.usda@])';
  const mapping = { 'https://assets.invalid/layers/a.usda': 'layers/a.usda', 'https://assets.invalid/textures/a.png': 'images/a.png' };
  const resolve = async path => encoder.encode(path.endsWith('.usda')
    ? '#usda 1.0\ndef Scope "A" { asset texture = @../textures/a.png@ }' : 'image');
  const result = await localizeProjectDependencies(source, new Map(), mapping, resolve);
  assert.match(new TextDecoder().decode(result.assets.get('layers/a.usda').bytes), /@\.\.\/images\/a.png@/);
  await assert.rejects(() => localizeProjectDependencies(source, new Map([['layers/a.usda', { bytes: encoder.encode('existing') }]]), mapping, resolve), { code: 'LUCIA_ASSET_COLLISION' });
});
test('typed generic primvars validate constant, uniform and indexed corner cardinality', async () => {
  const session = fakeSession('#usda 1.0\ndef Mesh "M" { point3f[] points = [(0,0,0),(1,0,0),(0,1,0)] int[] faceVertexCounts = [3] int[] faceVertexIndices = [0,1,2] }');
  await session.setPrimvar('/M', { name: 'label', itemSize: 1, array: new Int32Array([4]), interpolation: 'constant' });
  await session.setPrimvar('/M', { name: 'face', itemSize: 1, array: new Float64Array([1.123456789]), interpolation: 'uniform' });
  await session.setPrimvar('/M', { name: 'corners', itemSize: 2, array: new Int32Array([1, 2, 3, 4]), indices: new Uint32Array([0, 1, 0]), interpolation: 'faceVarying' });
  assert.match(session.usda, /int2\[\] primvars:corners/);
  assert.match(session.usda, /double\[\] primvars:face/);
  await assert.rejects(() => session.setPrimvar('/M', { name: 'bad', itemSize: 1, array: new Float32Array([1, 2]), interpolation: 'uniform' }), { code: 'LUCIA_MESH_ATTRIBUTE' });
});
test('LOD variants select one sibling mesh while preserving existing material variants', async () => {
  const session = fakeSession('#usda 1.0\ndef Xform "P" (prepend variantSets = "look" variants = { string look = "Red" }) { def Mesh "M" {} def Mesh "M_LOD1" {} variantSet "look" = { "Red" { over "M" { int appearance = 42 } } } }');
  await session.setLODVariants(['/P/M', '/P/M_LOD1']);
  assert.match(session.usda, /prepend variantSets = \["look", "luciaLOD"\]/);
  const composed = await composeProject(session.usda, new Map());
  assert.match(composed, /int appearance = 42/);
  const visible = findPrimBlock(composed, '/P/M');
  assert.doesNotMatch(composed.slice(visible.start, visible.end), /visibility = "invisible"/);
  assert.match(composed, /token visibility = "invisible"/);
  await assert.rejects(() => session.setLODVariants(['/P/M', '/P/M_LOD1']), { code: 'LUCIA_LOD_VARIANT' });
});
test('worker budgets count shared buffers once and abort settles only the active task', async () => {
  const buffer = new ArrayBuffer(64); assert.equal(workerPayloadBytes({ a: new Uint8Array(buffer), b: new Uint8Array(buffer) }), 64);
  assert.throws(() => workerPayloadBytes(buffer, 32), { code: 'LUCIA_WORKER_MEMORY' });
  const signal = new AbortController(), worker = { terminate() { this.terminated = true; } }, owner = { worker };
  let failure; monitorWorker(owner, worker, error => { failure = error; }, { signal: signal.signal, timeoutMs: 100 });
  signal.abort(); assert.equal(failure.code, 'LUCIA_CANCELLED'); assert.equal(owner.worker, null); assert.equal(worker.terminated, true);
});
test('rig frame diagnostics bound samples and require ordered times', () => {
  const identity = new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]), translated = identity.slice(); translated[12] = 2;
  const input = { positions: new Float32Array([0,0,0]), skinIndices: new Uint16Array([0,0,0,0]), skinWeights: new Float32Array([1,0,0,0]), poses: [{ time: 0, boneMatrices: identity }, { time: 1, boneMatrices: translated }] };
  const report = analyzeRigPoseSequence(input); assert.equal(report.maxDistance, 2); assert.equal(report.peakTime, 1);
  assert.throws(() => analyzeRigPoseSequence({ ...input, poses: [...input.poses].reverse() }), /increasing/);
});
test('native property history can undo and redo a typed attribute without whole source snapshots', async () => {
  const session = fakeSession('#usda 1.0\ndef Scope "P" { int value = 1 }');
  await session.nativeAttributeEdits([{ args: { path: '/P', attr_name: 'value', value: { type: 'int', value: 1 } } }], 'canonicalize');
  const history = new LuciaCommandStack();
  await history.execute(sessionCommand(session, 'value', ['/P'], () => session.nativeAttributeEdits([{ args: { path: '/P', attr_name: 'value', value: { type: 'int', value: 2 } } }], 'value')));
  assert.ok(history.undoItems[0].before.propertyDelta);
  await history.undo(); assert.match(session.usda, /int value = 1/);
  await history.redo(); assert.match(session.usda, /int value = 2/);
});
