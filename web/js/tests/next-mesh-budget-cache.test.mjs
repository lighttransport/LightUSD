// SPDX-License-Identifier: Apache-2.0
// Verify scoped Stage accounting against fresh scans while live components change.
import assert from 'node:assert/strict';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
const moduleUrl = process.env.LIGHTUSD_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_MODULE))
  : new URL(process.env.LIGHTUSD_WASM64 === '1'
      ? '../src/lightusd/lightusd_next_64.js'
      : '../src/lightusd/lightusd_next.js', import.meta.url);
const {default: createModule} = await import(moduleUrl);
const m = await createModule({print(){}, printErr(){}});
m._lightusd_next_render_mesh_copy_scope(0,-1,0);
m._lightusd_next_render_remaining_memory_bytes(0);
const scope=m._lightusd_next_render_mesh_copy_scope;
const remaining=m._lightusd_next_render_remaining_memory_bytes;
let handle=0;
m._lightusd_next_render_mesh_copy_scope=(h,id,b)=>{handle=h;return scope(h,id,b);};
const s=new m.RenderStream();
s.setMaxMemoryLimitMB(512);
const fixture = '#usda 1.0\ndef Mesh "Triangle" {\n' +
  'point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]\n' +
  'int[] faceVertexCounts = [3]\nint[] faceVertexIndices = [0,1,2]\n}\n' +
  Array.from({length: 128}, (_, i) => `def Xform "Extra${i}" {}\n`).join('');
s.begin(new TextEncoder().encode(fixture),'budget.usda');
assert(s.ok());
const owned = s.getMeshCopy(0);
const pointsBefore = Array.from(owned.points);
assert(handle);
assert.equal(scope(handle,0,1),0);
let checks=0;
function exactCold(label){
  const hot=remaining(handle);
  assert.equal(remaining(handle),hot,label+' repeated');
  assert.equal(scope(handle,0,0),0);
  assert.equal(scope(handle,0,1),0);
  assert.equal(remaining(handle),hot,label+' uncached');
  ++checks;
  return hot;
}
const initial=exactCold('initial');
s.setMaxMemoryLimitMB(576);
assert.equal(exactCold('limit increase'),initial+64*1024*1024);
s.setMaxMemoryLimitMB(512);
assert.equal(exactCold('limit restore'),initial);
const name='budget-extra.bin';
s.provideAsset(name,new Uint8Array(16));
assert.equal(exactCold('asset provide'),initial-name.length-16);
assert(s.removeAsset(name));
assert.equal(exactCold('asset remove'),initial);
s.provideAsset(name,new Uint8Array(32));
s.clearAssets();
assert.equal(exactCold('asset clear'),initial);
const exported=s.exportUSDCToBuffer(new Uint8Array(4*1024*1024),{});
assert(exported.success,JSON.stringify(exported));
const afterExport=exactCold('retained export');
assert(afterExport < initial);
// Force real linear-memory growth while a scope and owned output are alive.
// Request one whole current heap so the allocator cannot satisfy it in-place.
const oldHeap = m.HEAPU8.buffer;
const pressure = m._lightusd_next_alloc(m.HEAPU8.byteLength);
assert(pressure);
assert.notEqual(m.HEAPU8.buffer, oldHeap);
m._lightusd_next_free(pressure);
assert.equal(exactCold('heap growth'), afterExport);
assert.deepEqual(Array.from(owned.points), pointsBefore);
assert.equal(scope(handle,0,0),0);
assert.deepEqual(Array.from(s.getMeshCopy(0).points), pointsBefore);
s.delete();
console.log(JSON.stringify({checks,initial,afterExport,exportBytes:exported.size}));
