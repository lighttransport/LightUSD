import test from 'node:test';
import assert from 'node:assert/strict';
import { LuciaActivityGate } from '../src/activity-gate.js';

test('replacement waits until a cancelled edit releases its rollback lease',async()=>{
  let cancelled=0;
  const gate=new LuciaActivityGate(()=>cancelled++),edit=gate.acquire();
  const epoch=gate.beginReplacement();
  assert.equal(cancelled,1);
  assert.throws(()=>gate.acquire(),e=>e.code==='LUCIA_BUSY');
  let replacement;
  const waiting=gate.replacement(epoch).then(value=>{replacement=value;});
  await Promise.resolve();assert.equal(replacement,undefined);
  edit.release();await waiting;assert.ok(replacement);
  replacement.release();gate.finishReplacement(epoch);
  gate.acquire().release();
});
test('newest replacement supersedes slow imports and older waiters',async()=>{
  const gate=new LuciaActivityGate(),edit=gate.acquire();
  const first=gate.beginReplacement(),old=gate.replacement(first);
  const next=gate.beginReplacement(),latest=gate.replacement(next);
  edit.release();
  assert.equal(await old,null);
  const lease=await latest;assert.ok(lease);
  gate.finishReplacement(first);assert.equal(gate.pending,next);
  lease.release();gate.finishReplacement(next);
  assert.equal(gate.active,null);assert.equal(gate.pending,0);
});
test('an obsolete file read cannot acquire a scene replacement lease',async()=>{
  const gate=new LuciaActivityGate(),first=gate.beginReplacement(),next=gate.beginReplacement();
  const lease=await gate.replacement(next);lease.release();gate.finishReplacement(next);
  assert.equal(gate.current(first),false);assert.equal(await gate.replacement(first),null);
});
