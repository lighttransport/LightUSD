// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {buildUSDZWithNewRoot} from '../src/usdzconvert.js';
const cli=fileURLToPath(new URL('../cli/usd-to-mjcf.js',import.meta.url));
const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'lightusd-physics-cli-'));
const source=`#usda 1.0
(defaultPrim="World")
def Xform "World" {
 def Xform "Body" (prepend apiSchemas=["PhysicsRigidBodyAPI"]) {
  matrix4d xformOp:transform=((1,0,0,0),(0,1,0,0),(0,0,1,0),(20,30,40,1))
  uniform token[] xformOpOrder=["xformOp:transform"]
  def Xform "Offset" {
   double3 xformOp:translate=(1,2,3)
   uniform token[] xformOpOrder=["xformOp:translate"]
   def Mesh "Tet" (prepend apiSchemas=["PhysicsCollisionAPI"]) {
    matrix4d xformOp:transform=((-2,0,0,0),(0,3,0,0),(0,0,4,0),(5,6,7,1))
    uniform token[] xformOpOrder=["xformOp:transform"]
    uniform token subdivisionScheme="none"
    point3f[] points=[(0,0,0),(1,0,0),(0,1,0),(0,0,1)]
    int[] faceVertexCounts=[3,3,3,3]
    int[] faceVertexIndices=[0,2,1,0,1,3,0,3,2,1,2,3]
   }
  }
 }
}`;
const run=(input,...args)=>{
 const result=spawnSync(process.execPath,[cli,input,...args],{encoding:'utf8'});
 assert.equal(result.status,0,result.stderr+'\n'+result.stdout);return result;
};
try {
 const text=path.join(temporary,'scene.usda');fs.writeFileSync(text,source);
 const packed=path.join(temporary,'scene.usdz');
 fs.writeFileSync(packed,buildUSDZWithNewRoot('nested/root.usda',new TextEncoder().encode(source),[]));
 for(const [index,input] of [text,packed].entries()) {
  const dump=JSON.parse(run(input,'--dump-json').stdout);
  assert.ok(dump.prims.some(prim=>prim.path==='/World/Body/Offset/Tet' && prim.geometry?.type==='mesh'));
  const output=path.join(temporary,'out'+index+'.mjcf');run(input,'-o',output);
  const xml=fs.readFileSync(output,'utf8');assert.match(xml,/<geom[^>]*type="mesh"[^>]*pos="0 0 0"/);
  const obj=fs.readFileSync(path.join(temporary,'out'+index+'_assets','Tet.obj'),'utf8');
  assert.doesNotMatch(obj,/PLACEHOLDER/);
  const points=obj.split('\n').filter(line=>line.startsWith('v ')).map(line=>line.slice(2).split(/\s+/).map(Number));
  assert.deepEqual(points,[[6,8,10],[4,8,10],[6,11,10],[6,8,14]]);
  const faces=obj.split('\n').filter(line=>line.startsWith('f '));
  assert.equal(faces.length,4);assert.equal(faces[0],'f 1 2 3','mirroring reverses winding');
 }
 // Invalid render-only UVs cannot prevent independent physics extraction.
 const bad=path.join(temporary,'bad.usda');fs.writeFileSync(bad,source.replace('point3f[] points=', 'texCoord2f[] primvars:st=[(0,0)] (interpolation="faceVarying")\n    point3f[] points='));
 assert.ok(JSON.parse(run(bad,'--dump-json').stdout).prims.length>0);
 // Exercise actual exporter CLIs, not just their pure JS payload helpers.
 const exporter = fileURLToPath(new URL('../cli/urdf-to-usd.js',import.meta.url));
 const exported = spawnSync(process.execPath,[exporter,'--sample','--format','all','-o',path.join(temporary,'robot')],{encoding:'utf8',timeout:60000});
 assert.equal(exported.status,0,exported.stderr);assert.match(exported.stdout,/Verified LightUSDSampleRobot/);
 const roundtrip = fileURLToPath(new URL('../cli/usd-urdf-roundtrip.js',import.meta.url));
 for(const format of ['usda','usdc']) {
  const result = spawnSync(process.execPath,[roundtrip,'--sample','--format',format],{encoding:'utf8',timeout:60000});
  assert.equal(result.status,0,result.stderr);assert.match(result.stdout,/Verified USD Physics/);
 }
 console.log('PASS: core physics CLI extraction, USDZ, nested/mirrored body-local mesh geometry');
} finally {fs.rmSync(temporary,{recursive:true,force:true});}
