// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
const suffix=process.env.LIGHTUSD_WASM64==='1'?'_64':'';
const {default:factory}=await import(`../src/lightusd/lightusd${suffix}.js`);
const core=await factory(),geometry=await core.loadFeature('geometry');
const positions=new Float32Array([0,0,0,1,0,0,1,1,0,0,1,0]);
const indices=new Uint32Array([0,1,2,0,2,3]);
const atlas=new geometry.XAtlasNative();
for(const options of [{},{resolution:64,padding:2,rotateCharts:false,blockAlign:true},{maxIterations:2,maxCost:3,textureSeamWeight:1}]) {
  const result=atlas.generate(positions,indices,options);
  assert.ok(!result.error,result.error);
  assert.equal(result.indexCount,6);assert.equal(result.sourceVertexCount,4);
  assert.equal(result.atlasCount,1);assert.ok(result.chartCount>0);
  assert.ok(result.width>0 && result.height>0);
  assert.notEqual(result.positions.buffer,geometry.HEAPU8.buffer);
  for(let i=0;i<result.vertexCount;i++) {
    assert.ok(result.xref[i]<4);
    assert.deepEqual([...result.positions.subarray(i*3,i*3+3)],
      [...positions.subarray(result.xref[i]*3,result.xref[i]*3+3)]);
    assert.ok(result.uvs[i*2]>=0 && result.uvs[i*2]<=1);
    assert.ok(result.uvs[i*2+1]>=0 && result.uvs[i*2+1]<=1);
    assert.equal(result.atlasIndices[i],0);
    assert.ok(result.chartIndices[i]>=0);
  }
  assert.ok([...result.indices].every(index=>index<result.vertexCount));
  for(let face=0;face<6;face+=3) {
    const [a,b,c]=[...result.indices.subarray(face,face+3)].map(index=>[result.uvs[index*2],result.uvs[index*2+1]]);
    assert.ok(Math.abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))>0);
  }
}
// Charting normalizes finite coordinates before float cross products, while
// retaining the exact authored positions in the owned output.
for (const scale of [1e-30,1e25]) {
  const input=Float32Array.from(positions,value=>value*scale);
  const result=atlas.generate(input,indices);
  assert.ok(!result.error,result.error);
  for(let i=0;i<result.vertexCount;i++)
    assert.deepEqual([...result.positions.subarray(i*3,i*3+3)],
      [...input.subarray(result.xref[i]*3,result.xref[i]*3+3)]);
}
assert.ok(atlas.generate(positions,new Uint32Array([0,1,99])).error);
assert.ok(atlas.generate(new Float32Array([NaN,...positions.subarray(1)]),indices).error);
assert.throws(()=>atlas.generate(positions,indices,{resolution:1e9}),/resolution/);
assert.throws(()=>atlas.generate(positions,indices,{rotateCharts:1}),/rotateCharts/);
assert.throws(()=>atlas.generate(new Float64Array(positions),indices),/Float32Array/);
const zero=geometry._lu_feature_info(2)===64?0n:0;
assert.equal(Number(geometry._lu_geometry_xatlas(zero,12,zero,6,zero)),0);
atlas.delete();assert.throws(()=>atlas.generate(positions,indices),/deleted/);
// Exercise the actual Lucia worker handler, including xref remapping and the
// owned ArrayBuffers it transfers across the browser Worker boundary.
let posted;
globalThis.self={postMessage:(message,transfers)=>{posted={message,transfers};}};
await import('../lucia-code/src/xatlas-worker.js');
const normals=new Float32Array([0,0,1,0,0,1,0,0,1,0,0,1]);
await self.onmessage({data:{type:'unwrap',positions,indices,normals,options:{resolution:64}}});
assert.equal(posted.message.type,'result',posted.message.message);
assert.ok(posted.transfers.length>0);
assert.ok(posted.transfers.every(buffer=>buffer instanceof ArrayBuffer));
assert.equal(posted.message.result.normals.length,posted.message.result.positions.length);
console.log('PASS: core xatlas options, UVs, source mappings, ownership and Lucia worker');
