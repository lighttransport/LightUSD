// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
const suffix=process.env.LIGHTUSD_WASM64==='1'?'_64':'';
const {default:createCore}=await import(`../src/lightusd/lightusd${suffix}.js`);
const core=await createCore();
const points=300000,faces=100000;
const array=(value,count)=>'['+Array(count).fill(value).join(',')+']';
const source=new TextEncoder().encode(`#usda 1.0
(defaultPrim="Body")
def Mesh "Body" (prepend apiSchemas=["PhysicsRigidBodyAPI","PhysicsCollisionAPI"]) {
 point3f[] points=${array('(0,0,0)',points)}
 int[] faceVertexCounts=${array('3',faces)}
 int[] faceVertexIndices=${array('0,1,2',faces)}
 texCoord2f[] primvars:st=${array('(0,0)',points)} (interpolation="vertex")
 color3f[] primvars:custom=${array('(1,0,0)',points)} (interpolation="vertex")
 point3f[] extent=[(-1,-1,-1),(1,1,1)]
 double3 xformOp:translate=(1,2,3)
 uniform token[] xformOpOrder=["xformOp:translate"]
 float physics:mass=2
 float[] mjc:diagnostic=[1,2,3]
 float[] diagnosticSamples=[1.25,2.5]
 rel physics:filteredPairs=[</Body>]
}
`);
const converter=new core.NextUSDZConverterNative();
try {
 assert.ok(converter.loadFromBinary(source,'large.usda'),converter.error());
 for(const binary of [false,true]) {
  if(binary) {
   const crate=new Uint8Array(converter.exportAsUSDC());assert.ok(crate.length);
   assert.ok(converter.loadFromBinary(crate,'large.usdc'),converter.error());
  }
  const before=core.HEAPU8.byteLength;
  const text=converter.extractPhysicsSceneJSON();assert.ok(text,converter.error());
  assert.ok(text.length<16000,`Physics snapshot expanded render arrays: ${text.length}`);
  assert.ok(core.HEAPU8.byteLength-before<16*1024*1024,'Physics extraction grew the WASM heap excessively');
  const snapshot=JSON.parse(text),mesh=snapshot.prims.find(prim=>prim.path==='/Body');
  assert.equal(mesh.geometry.type,'mesh');assert.equal(mesh.geometry.pointCount,points);
  assert.equal(mesh.geometry.faceCount,faces);
  for(const key of ['positions','indices','normals','uvs'])assert.equal(mesh.geometry[key],undefined);
  for(const key of ['points','faceVertexCounts','faceVertexIndices','primvars:st','primvars:custom'])assert.equal(mesh.properties[key],undefined);
  assert.equal(mesh.properties['physics:mass'],2);
  assert.deepEqual(mesh.properties['mjc:diagnostic'],[1,2,3]);
  assert.deepEqual(mesh.properties.diagnosticSamples,[1.25,2.5]);
  assert.deepEqual(mesh.properties.xformOpOrder,['xformOp:translate']);
  assert.deepEqual(mesh.properties.extent,[-1,-1,-1,1,1,1]);
  assert.deepEqual(mesh.relationships['physics:filteredPairs'],['/Body']);
 }
 console.log('PASS compact physics snapshots: large USDA/USDC mesh counts, bounded memory, engine arrays and transforms');
}finally{converter.delete();}
