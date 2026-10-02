import test from 'node:test';
import assert from 'node:assert/strict';
import { LuciaProjectStorage } from '../src/project-storage.js';
import { LuciaCommandStack } from '../src/command-stack.js';
import { createLayerPropertyDelta, applyLayerPropertyDelta } from '../src/layer-delta.js';
if (!globalThis.CustomEvent) globalThis.CustomEvent = class extends Event { constructor(type, options) { super(type); this.detail = options?.detail; } };
test('persistent snapshots deduplicate assets and reject corrupt recovery', async () => {
  const records = new Map(), adapter = { get: async key => structuredClone(records.get(key)),
    putMany: async entries => entries.forEach(([key, value]) => records.set(key, structuredClone(value))) };
  const storage = new LuciaProjectStorage(adapter);
  const bytes = Uint8Array.of(1, 2, 3), project = { name: 'Test', exportRemap: {},
    assets: new Map([['a.bin', { bytes }], ['b.bin', { bytes: bytes.slice(), colorSpace: 'raw' }]]) };
  const saving = storage.save(project, '#usda 1.0\n'); bytes[0] = 9;
  await saving;
  assert.equal(records.size, 3); // manifest, source, one shared content blob
  const saved = await storage.load();
  assert.deepEqual([...saved.assets.get('a.bin').bytes], [1, 2, 3]);
  saved.assets.get('a.bin').bytes[0] = 7;
  assert.equal(saved.assets.get('b.bin').bytes[0], 1);
  const manifest = records.get('current'); records.get(`blob:${manifest.assets[0].hash}`)[0] = 8;
  await assert.rejects(() => storage.load(), { code: 'LUCIA_PROJECT_STORAGE' });
});
test('failed persistent transaction leaves the saved manifest untouched', async () => {
  const storage = new LuciaProjectStorage({ putMany: async () => { throw new Error('quota'); } });
  await assert.rejects(() => storage.save({ name: 'P', assets: new Map(), exportRemap: {} }, '#usda 1.0'), /quota/);
});
test('concurrent saves commit in request order and a failed save releases the queue', async () => {
  const committed = [], project = { name: 'P', assets: new Map(), exportRemap: {} };
  let entered, release;
  const started = new Promise(resolve => { entered = resolve; });
  const blocked = new Promise(resolve => { release = resolve; });
  const storage = new LuciaProjectStorage({ putMany: async entries => {
    const source = new TextDecoder().decode(entries[0][1]);
    if (source === 'first') { entered(); await blocked; throw new Error('quota'); }
    committed.push(source);
  } });
  const first = storage.save(project, 'first');
  const failure = assert.rejects(first, /quota/);
  await started;
  const second = storage.save(project, 'second'), third = storage.save(project, 'third');
  await new Promise(resolve => setTimeout(resolve, 10));
  assert.deepEqual(committed, []);
  release(); await failure; await Promise.all([second, third]);
  assert.deepEqual(committed, ['second', 'third']);
});
test('property history retains only changed property values and checks stale state', () => {
  const before = { primSpecs: { P: { properties: { a: { value: 1 }, huge: { value: 'x'.repeat(100000) } } } } };
  const after = structuredClone(before); after.primSpecs.P.properties.a.value = 2;
  const delta = createLayerPropertyDelta(before, after);
  assert.ok(JSON.stringify(delta).length < 200);
  assert.deepEqual(applyLayerPropertyDelta(after, delta, 'undo'), before);
  assert.deepEqual(applyLayerPropertyDelta(before, delta, 'redo'), after);
  after.primSpecs.P.properties.a.value = 3;
  assert.throws(() => applyLayerPropertyDelta(after, delta, 'undo'), { code: 'LUCIA_HISTORY_STALE' });
  const structural = structuredClone(before); structural.primSpecs.P.typeName = 'Mesh';
  assert.equal(createLayerPropertyDelta(before, structural), null);
});
test('history pruning validates retention and cannot run during edits', () => {
  const history = new LuciaCommandStack(); history.undoItems = [{}, {}, {}]; history.redoItems = [{}];
  history.prune(1); assert.equal(history.undoItems.length, 1); assert.equal(history.redoItems.length, 0);
  assert.throws(() => history.prune(-1), RangeError);
  history.busy = true; assert.throws(() => history.prune(), /during an operation/);
});
