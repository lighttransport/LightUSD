import test from 'node:test';
import assert from 'node:assert/strict';
import { createSourceDelta, applySourceDelta } from '../src/source-delta.js';
import { LuciaCommandStack, sessionCommand, estimateHistoryBytes } from '../src/command-stack.js';
globalThis.CustomEvent ||= class extends Event { constructor(type, options) { super(type); this.detail = options?.detail; } };

test('small edits retain only changed source and round-trip Unicode', async () => {
  const prefix = '#usda 1.0\n' + '# unchanged\n'.repeat(100000);
  let source = prefix + 'def Xform "世界" {}';
  const before = source, after = prefix + 'def Xform "世界🌍" {}';
  const session = { exportUSDA: () => source, restore: async value => { source = value; } };
  const stack = new LuciaCommandStack();
  await stack.execute(sessionCommand(session, 'Rename', [], async () => { source = after; }));
  assert.ok(estimateHistoryBytes(stack.undoItems) < 1024);
  await stack.undo(); assert.equal(source, before);
  await stack.redo(); assert.equal(source, after);
  source = source.replace('世界', 'changed');
  const stale = source;
  await assert.rejects(() => stack.undo(), { code: 'LUCIA_HISTORY_STALE' });
  assert.equal(source, stale); assert.equal(stack.undoItems.length, 1);
});
test('damaged deltas and invalid history budgets fail closed', async () => {
  const delta = await createSourceDelta('before', 'after');
  await assert.rejects(() => applySourceDelta('before', { ...delta, inserted: 'damaged' }, 'redo'), { code: 'LUCIA_HISTORY' });
  for (const maxBytes of [-1, 0, NaN, Infinity]) assert.throws(() => new LuciaCommandStack({ maxBytes }), RangeError);
  const emoji = await createSourceDelta('😀', '😁');
  assert.equal(await applySourceDelta('😀', emoji, 'redo'), '😁');
  assert.equal(await applySourceDelta('😁', emoji, 'undo'), '😀');
});

test('distant edits retain separate intervals and account for shifted offsets', async () => {
  const middle = '# unchanged geometry\n'.repeat(100000);
  const before = 'name = "first"\n' + middle + 'name = "last"\n';
  const after = 'name = "first 🌍 renamed"\n' + middle + 'name = "x"\n';
  const delta = await createSourceDelta(before, after);
  assert.equal(delta.edits.length, 2);
  assert.ok(estimateHistoryBytes([{ before: delta }]) < 1024);
  assert.equal(await applySourceDelta(before, delta, 'redo'), after);
  assert.equal(await applySourceDelta(after, delta, 'undo'), before);
  await assert.rejects(() => applySourceDelta(before, { ...delta, edits: [...delta.edits].reverse() }, 'redo'), { code: 'LUCIA_HISTORY' });
  const newLines = await createSourceDelta(before, after.replace('renamed', 'renamed\nextra'));
  assert.equal(newLines.edits, undefined);
  assert.equal(await applySourceDelta(after.replace('renamed', 'renamed\nextra'), newLines, 'undo'), before);
});

test('independent equal assets and restored copies share immutable history storage', async () => {
  let source = 'before';
  const session = { exportUSDA: () => source, restore: async value => { source = value; } };
  const project = { assets: new Map([['a', { bytes: new Uint8Array(1048576) }], ['b', { bytes: new Uint8Array(1048576) }]]), exportRemap: {}, emit() {} };
  const stack = new LuciaCommandStack();
  await stack.execute(sessionCommand(session, 'Edit', [], async () => { source = 'after'; }, project));
  const snapshot = stack.undoItems[0].projectBefore;
  assert.equal(snapshot.assets.get('a').bytes.buffer, snapshot.assets.get('b').bytes.buffer);
  await stack.undo(); await stack.redo();
  project.assets.get('b').bytes[0] = 1;
  await stack.execute(sessionCommand(session, 'Edit again', [], async () => { source = 'new'; }, project));
  const next = stack.undoItems[1].projectBefore;
  assert.equal(next.assets.get('a').bytes.buffer, snapshot.assets.get('a').bytes.buffer);
  assert.notEqual(next.assets.get('b').bytes.buffer, snapshot.assets.get('b').bytes.buffer);
  assert.equal(snapshot.assets.get('b').bytes[0], 0);
});
