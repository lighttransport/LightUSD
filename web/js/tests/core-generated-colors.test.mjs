// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {NextRenderSceneAdapter} from '../src/lightusd/LightUSDLoader.js';
import {buildNextThreeNode} from '../src/lightusd/NextRenderSceneUtils.js';
const suffix=process.env.LIGHTUSD_WASM64==='1'?'_64':'';
const {default:factory}=await import(`../src/lightusd/lightusd${suffix}.js`);
const core=await factory();
const source=new TextEncoder().encode(`#usda 1.0
 def Xform "World" {
  def Sphere "Ball" {
   color3f[] primvars:displayColor=[(1,0.25,0.5)] (interpolation="constant")
   float[] primvars:displayOpacity=[0.4] (interpolation="constant")
  }
  def Cube "Box" {
   color3f[] primvars:displayColor=[(1,0,0),(0,1,0),(0,0,1)] (interpolation="uniform")
   int[] primvars:displayColor:indices=[0,1,2,0,1,2]
  }
 }`);
const adapter=await NextRenderSceneAdapter.create(core,source,'colors.usda');
try {
  const ball=adapter.meshes.find(mesh=>mesh.primPath==='/World/Ball');
  const box=adapter.meshes.find(mesh=>mesh.primPath==='/World/Box');
  assert.ok(ball && box);
  assert.equal(ball.vertexColors.length,ball.points.length);
  assert.equal(ball.opacities.length,ball.points.length/3);
  for(let i=0;i<ball.points.length/3;i++) {
    assert.deepEqual([...ball.vertexColors.subarray(i*3,i*3+3)],[1,0.25,0.5]);
    assert.ok(Math.abs(ball.opacities[i]-0.4)<1e-6);
  }
  assert.equal(box.vertexColors.length,box.points.length);
  const seen=new Set();
  for(let i=0;i<box.indices.length;i+=3) {
    const colors=[...box.indices.subarray(i,i+3)].map(index=>[...box.vertexColors.subarray(index*3,index*3+3)].join(','));
    assert.equal(colors[0],colors[1]);assert.equal(colors[1],colors[2]);seen.add(colors[0]);
  }
  assert.deepEqual([...seen].sort(),['0,0,1','0,1,0','1,0,0']);
  const {node}=buildNextThreeNode(adapter,{skipTextures:true,releaseBuildData:false});
  let painted;
  node.traverse(object=>{if(object.isMesh && object.userData['primMeta.absPath']==='/World/Ball')painted=object;});
  assert.ok(painted);
  assert.equal(painted.geometry.attributes.color.itemSize,4);
  assert.equal(painted.geometry.attributes.color.count,painted.geometry.attributes.position.count);
  assert.equal(painted.material.vertexColors,true);
  assert.equal(painted.material.transparent,true);
  node.traverse(object=>{object.geometry?.dispose();if(object.material){for(const material of Array.isArray(object.material)?object.material:[object.material])material.dispose();}});
} finally {adapter.delete();}
console.log('PASS: generated constant/indexed-uniform color and opacity survive core conversion and Three.js preview');
