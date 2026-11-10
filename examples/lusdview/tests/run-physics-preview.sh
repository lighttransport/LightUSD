#!/usr/bin/env bash
set -euo pipefail
BIN="${LUSDVIEW:?LUSDVIEW is required}"
command -v node >/dev/null || { echo 'SKIP: node unavailable'; exit 77; }
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
cat > "$OUT/scene.usda" <<'USD'
#usda 1.0
(
 upAxis="Z"
 metersPerUnit=1
)
def PhysicsScene "Scene" {
 vector3f physics:gravityDirection=(0,0,-1)
 float physics:gravityMagnitude=9.81
}
def Plane "Ground" (prepend apiSchemas=["PhysicsCollisionAPI"]) {
 uniform token axis="Z"
}
def Material "Red" {
 token outputs:surface.connect=</Red/P.outputs:surface>
 def Shader "P" {
  uniform token info:id="UsdPreviewSurface"
  color3f inputs:diffuseColor=(0.8,0.02,0.01)
  token outputs:surface
 }
}
def Xform "Parent" {
 double3 xformOp:translate=(10,20,0)
 uniform token[] xformOpOrder=["xformOp:translate"]
 def Sphere "Ball" (prepend apiSchemas=["PhysicsRigidBodyAPI","PhysicsCollisionAPI"]) {
  double radius=1
  rel material:binding=</Red>
  double3 xformOp:translate=(0,0,4)
  uniform token[] xformOpOrder=["xformOp:translate"]
 }
}
USD
printf '%s\n' '{"window_size":{"width":128,"height":128}}' > "$OUT/config.json"
node - "$BIN" "$OUT" <<'NODE'
const {spawn}=require('node:child_process');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const child=spawn(process.argv[2],['--headless','--backend','gl','--no-threaded','--frames','100000','--config',process.argv[3]+'/config.json','--mcp-stdio',process.argv[3]+'/scene.usda'],{stdio:['pipe','pipe','pipe']});
let stdout='',stderr='',id=0;const pending=new Map();
child.stderr.on('data',data=>{stderr+=data;});
child.stdout.on('data',data=>{stdout+=data;for(;;){const end=stdout.indexOf('\n');if(end<0)break;const line=stdout.slice(0,end);stdout=stdout.slice(end+1);let reply;try{reply=JSON.parse(line);}catch{continue;}const waiter=pending.get(reply.id);if(!waiter)continue;pending.delete(reply.id);clearTimeout(waiter.timer);if(reply.error||reply.result?.isError)waiter.reject(Error(JSON.stringify(reply.error||reply.result)));else waiter.resolve(reply.result?.structuredContent);}});
function call(name,args={}){return new Promise((resolve,reject)=>{const request=++id;const timer=setTimeout(()=>{pending.delete(request);reject(Error(name+' timed out\n'+stderr));},30000);pending.set(request,{resolve,reject,timer});child.stdin.write(JSON.stringify({jsonrpc:'2.0',id:request,method:'tools/call',params:{name,arguments:args}})+'\n');});}
async function capture(name){
 await call('set_focus',{path:''});
 await call('vchar_physics',{op:'hide'});
 let barrier;
 for(let i=0;i<100;++i){barrier=await call('frame_barrier',{token:name});if(barrier.ready)break;await new Promise(resolve=>setTimeout(resolve,10));}
 assert.equal(barrier.ready,true);
 const path=process.argv[3]+'/'+name+'.ppm';await call('screenshot',{path,barrier:name});
 const bytes=fs.readFileSync(path);const match=/^P6\s+(\d+)\s+(\d+)\s+255\s/.exec(bytes.toString('latin1'));assert.ok(match);
 const width=+match[1],pixels=bytes.subarray(match[0].length);let count=0,y=0;
 for(let i=0;i<pixels.length;i+=3)if(pixels[i]>80&&pixels[i]>pixels[i+1]*2&&pixels[i]>pixels[i+2]*2){++count;y+=Math.floor(i/3/width);}
 assert.ok(count>100,'red sphere pixels missing: '+count);return y/count;
}
(async()=>{
 const initial=await call('get_scene_info');assert.equal(initial.loaded,true);
 const capabilities=await call('vchar_status');assert.equal(capabilities.physics.loader,'core');assert.equal(capabilities.physics.simulation,true);
 assert.equal((await call('vchar_physics',{op:'status'})).initialized,false);
 const setup=await call('vchar_physics',{op:'initialize'});assert.equal(setup.initialized,true);assert.equal(setup.body_count,2);assert.equal(setup.collider_count,2);
 await call('render_settings',{mode:'albedo',grid:false,background:'black'});
 await call('viewport',{op:'set',target:[10,20,2.5],yaw:0,pitch:0,distance:8});
 const initialPixels=await capture('before');
 const before=await call('set_focus',{path:'/Parent/Ball'});assert.equal(before.world[12],10);assert.equal(before.world[13],20);assert.equal(before.world[14],4);
 const moved=await call('vchar_physics',{op:'step',steps:240,timestep:1/120});const ball=moved.bodies.find(x=>x.path==='/Parent/Ball');assert.ok(ball.position[2]>0.5&&ball.position[2]<2,JSON.stringify(moved));assert.equal(moved.mesh_sync,true);
 const after=await call('get_focused_prim');assert.ok(Math.abs(after.world[14]-ball.position[2])<1e-5,JSON.stringify(after));assert.equal(after.world[12],10);assert.equal(after.world[13],20);
 assert.ok(after.aabb_max[2]<before.aabb_max[2]-1);
 const movedPixels=await capture('moved');assert.ok(Math.abs(movedPixels-initialPixels)>10,'sphere did not move in GPU pixels');
 await assert.rejects(call('vchar_physics',{op:'step',steps:601}));
 assert.equal((await call('vchar_physics',{op:'hide'})).visible,false);
 assert.equal((await call('vchar_physics',{op:'status'})).visible,false);
 const reset=await call('vchar_physics',{op:'reset'});assert.equal(reset.bodies.find(x=>x.path==='/Parent/Ball').position[2],4);
 assert.equal((await call('set_focus',{path:'/Parent/Ball'})).world[14],4);
 const resetPixels=await capture('reset');assert.ok(Math.abs(resetPixels-initialPixels)<1,'GPU reset pose differs');
 console.log('PASS core MCP physics: initialization, falling contact, GPU pixels and world/bounds writeback, validation, hide/status and reset');
})().catch(error=>{console.error(error.stack+'\n'+stderr);process.exitCode=1;}).finally(()=>{child.kill('SIGTERM');for(const waiter of pending.values())clearTimeout(waiter.timer);});
NODE
