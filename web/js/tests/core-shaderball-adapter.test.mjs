// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import puppeteer from 'puppeteer';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const hardware=production && process.argv.includes('--hardware');
const config={root:new URL('..',import.meta.url).pathname,server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);if(!production)await server.listen();
const root=`#usda 1.0
def Xform "standard_shader_ball_scene" (
 variants={string surface_geometry="wrong"}
 prepend variantSets="surface_geometry"
) {
 variantSet "surface_geometry"={
  "triangulated" {
   def Xform "PartA" (references=@a/part.usda@</Asset>) {}
   def Xform "PartB" (references=@b/part.usda@</Asset>) {}
  }
  "wrong" { def Scope "Wrong" {} }
 }
 def Camera "Camera" { float focalLength=50; }
}`;
const part=color=>`#usda 1.0
def Xform "Asset" {
 def Mesh "Triangle" {
  uniform token subdivisionScheme="none"
  point3f[] points=[(-1,-1,0),(1,-1,0),(0,1,0)]
  int[] faceVertexCounts=[3]
  int[] faceVertexIndices=[0,1,2]
  texCoord2f[] primvars:st=[(0,0),(1,0),(0.5,1)] (interpolation="vertex")
  float[] primvars:custom=[1,2,3] (interpolation="vertex")
  rel material:binding=</Asset/Look>
 }
 def Material "Look" {
  token outputs:surface.connect=</Asset/Look/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id="UsdPreviewSurface"
   color3f inputs:diffuseColor=${color}
   asset inputs:diagnostic=@same.png@
   token outputs:surface
  }
 }
}`;
let browser;
try {
 browser=await puppeteer.launch({headless:!hardware,protocolTimeout:600000,args:[
  '--no-sandbox','--disable-dev-shm-usage','--disable-gpu-watchdog','--enable-unsafe-webgpu',
  '--enable-features=Vulkan','--use-angle=vulkan','--disable-vulkan-surface',
  ...(hardware?['--use-vulkan=native','--ignore-gpu-blocklist']:['--enable-unsafe-swiftshader','--use-vulkan=swiftshader','--use-webgpu-adapter=swiftshader'])]});
 const page=await browser.newPage();let imageRequests=0;
 page.on('pageerror',e=>console.error('PAGE:',e.message.slice(0,1000)));
 page.on('console',m=>{if(m.type()==='error')console.error('CONSOLE:',m.text().slice(0,1000));});
 await page.evaluateOnNewDocument(()=>{
  window.__buildEvents=[];
  const OriginalWorker=window.Worker;
  window.Worker=class extends OriginalWorker {constructor(...args){super(...args);window.__buildEvents.push('worker:'+args[0]);this.addEventListener('message',()=>window.__buildEvents.push('worker:message'));this.addEventListener('error',e=>window.__buildEvents.push('worker:error:'+e.message));}};
  for(const [type,method] of [[window.GPUDevice,'createComputePipelineAsync'],[window.GPUDevice,'createRenderPipelineAsync'],[window.GPUShaderModule,'getCompilationInfo']]) {
   if(!type)continue;const original=type.prototype[method];
   type.prototype[method]=function(...args){window.__buildEvents.push(method+':start');return original.apply(this,args).then(value=>{window.__buildEvents.push(method+':done');return value;});};
  }
 });
 await page.setRequestInterception(true);
 page.on('request',req=>{
  const path=new URL(req.url()).pathname;
  if(path==='/__adapter__.html')return req.respond({status:200,contentType:'text/html',body:'<!doctype html><title>Core adapter test</title>'});
  if(path.endsWith('/standard_shader_ball_scene.usda'))return req.respond({status:200,body:root});
  if(path.endsWith('/a/part.usda'))return req.respond({status:200,body:part('(1,0,0)')});
  if(path.endsWith('/b/part.usda'))return req.respond({status:200,body:part('(0,1,0)')});
  if(path.endsWith('/same.png')){imageRequests++;return req.respond({status:404});}
  req.continue();
 });
 // A blank intercepted page keeps this test independent of renderer startup.
 const url=`http://127.0.0.1:${server.httpServer.address().port}/__adapter__.html`;
 let result;
 if(production) {
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/webgpu-mtlx.html?manual&width=64&height=64`);
  try{await page.waitForFunction(()=>window.__webgpuMtlx?.ready||window.__webgpuMtlx?.errors.length,{timeout:180000});}catch(error){console.error(await page.evaluate(()=>({url:location.href,state:window.__webgpuMtlx?{ready:window.__webgpuMtlx.ready,errors:window.__webgpuMtlx.errors,renderer:!!window.__webgpuMtlx.renderer}:null,status:document.querySelector('#status')?.textContent,events:window.__buildEvents})));throw error;}
  await page.evaluate(()=>{
   const state=window.__webgpuMtlx;if(state.errors.length)throw Error(state.errors.join(';'));
   const renderer=state.renderer,load=renderer.loadScene.bind(renderer);
   renderer.loadScene=async scene=>{
    window.__adapterInput=structuredClone({triangles:scene.indices.length/3,
     uvs:scene.uvSets.map(set=>Array.from(set)),materials:scene.authored.materials.length,
     textures:scene.authored.textureSources.map(x=>x.authored),graph:scene.authored.shadingGraph,
     snapshots:Object.keys(scene.authored.primvarSnapshots),bindings:scene.authored.bindings,camera:scene.camera});
    return load(scene);
   };
   state.ready=false;const select=document.querySelector('#scene');select.value='shaderball';select.dispatchEvent(new Event('change'));
  });
  await page.waitForFunction(()=>window.__webgpuMtlx.ready||window.__webgpuMtlx.errors.length,{timeout:180000});
  result=await page.evaluate(async()=>{
   const state=window.__webgpuMtlx;await state.renderer.device.queue.onSubmittedWorkDone();
   if(state.errors.length)throw Error(state.errors.join(';'));
   return {...window.__adapterInput,packedTriangles:state.renderer.scene.triangleCount,adapter:state.renderer.stats.adapter};
  });
  assert.equal(result.packedTriangles,2);
 }else {
 await page.goto(url);
 result=await page.evaluate(async()=>{
  const {loadShaderBallGeometry}=await import('/src/webgpu-mtlx/usd-scene.js');
  const scene=await loadShaderBallGeometry();
  return {triangles:scene.indices.length/3,uvs:scene.uvSets,materials:scene.authored.materials.length,
   textures:scene.authored.textureSources.map(x=>x.authored),graph:scene.authored.shadingGraph,
   snapshots:Object.keys(scene.authored.primvarSnapshots),bindings:scene.authored.bindings,camera:scene.camera};
 });
 }
 if(hardware)assert.equal(result.adapter.isFallbackAdapter,false,'Expected a hardware WebGPU adapter');
 assert.equal(result.triangles,2);assert.ok(result.materials>=2);
 assert.ok(result.textures.some(x=>x.endsWith('/a/same.png')));assert.ok(result.textures.some(x=>x.endsWith('/b/same.png')));
 assert.equal(imageRequests,0);assert.equal(result.snapshots.length,2);assert.equal(result.bindings.length,2);
 assert.ok(result.uvs[0].length>0);assert.ok(result.camera.fov>0);
 assert.ok(!result.graph.prims.some(x=>x.path.endsWith('/Wrong')));
 console.log('PASS core ShaderBall adapter: variants, external layers, shading graphs, texture provenance, UVs and primvars');
}finally{await browser?.close();await server.close();}
