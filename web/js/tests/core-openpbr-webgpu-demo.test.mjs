// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import puppeteer from 'puppeteer';
import {PNG} from 'pngjs';
import {buildUSDZWithNewRoot} from '../src/usdzconvert.js';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const config={root:new URL('..',import.meta.url).pathname,server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);if(!production)await server.listen();
let browser;
try {
 browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-webgpu','--enable-unsafe-swiftshader','--enable-features=Vulkan','--use-angle=vulkan','--use-vulkan=swiftshader','--use-webgpu-adapter=swiftshader','--disable-vulkan-surface']});
 const page=await browser.newPage();const errors=[];
 page.on('pageerror',e=>{errors.push(e.message);console.error('PAGE:',e.message.slice(0,500));});page.on('console',m=>{if(m.type()==='error')errors.push(m.text());});
 await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/mtlx-webgpu.html`);
 await page.waitForFunction(()=>window._debugOpenPBRWebGPU?.loader?.native_,{timeout:60000});
 await page.waitForFunction(()=>/Ready|Error:/.test(document.querySelector('#status').textContent),{timeout:60000});
 const textured=new TextEncoder().encode(`#usda 1.0
def Mesh "Quad" {
 uniform token subdivisionScheme="none"
 point3f[] points=[(-1,-1,0),(1,-1,0),(0,1,0)]
 int[] faceVertexCounts=[3]
 int[] faceVertexIndices=[0,1,2]
 texCoord2f[] primvars:st=[(0,0),(1,0),(0.5,1)] (interpolation="vertex")
 rel material:binding=</Look>
}
def Material "Look" {
 token outputs:surface.connect=</Look/Surface.outputs:surface>
 def Shader "Surface" {
  uniform token info:id="UsdPreviewSurface"
  color3f inputs:diffuseColor.connect=</Look/Tex.outputs:rgb>
  token outputs:surface
 }
 def Shader "Tex" {
  uniform token info:id="UsdUVTexture"
  asset inputs:file=@red.png@
  float3 outputs:rgb
 }
}`);
 const archive=buildUSDZWithNewRoot('root.usda',textured,[{name:'red.png',data:PNG.sync.write({width:1,height:1,data:Buffer.from([255,0,0,255])})}]);
 const result=await page.evaluate(async(archive)=>{
  const app=window._debugOpenPBRWebGPU;
  const bytes=new TextEncoder().encode('#usda 1.0\ndef Cube "Box" { double3 xformOp:translate=(7,2,-3); double3 xformOp:scale=(2,3,4); uniform token[] xformOpOrder=["xformOp:translate","xformOp:scale"]; }');
  const instances=[],bounds=[];
  const update=app.renderer.updateInstances.bind(app.renderer),fit=app.controls.fitToBox.bind(app.controls);
  app.renderer.updateInstances=value=>{instances.push(value);return update(value);};
  app.controls.fitToBox=(min,max)=>{bounds.push({min,max});return fit(min,max);};
  const device=app.renderer.device;device.pushErrorScope('validation');
  await app.loadUSDFromData(bytes,'cube.usda');
  await device.queue.onSubmittedWorkDone();
  const mesh=app.nativeLoader.getMeshCopy(0);
  const first=app.renderer.meshes.length;
  await app.loadUSDFromData(bytes,'cube.usda');
  const uploads=[];const loadTexture=app.renderer.loadTexture.bind(app.renderer);
  app.renderer.loadTexture=(data,width,height,format)=>{uploads.push({pixels:Array.from(data),width,height,format});return loadTexture(data,width,height,format);};
  await app.loadUSDFromData(new Uint8Array(archive),'textured.usdz');
  await device.queue.onSubmittedWorkDone();
  const error=await device.popErrorScope();
  const images=app.nativeLoader.numImages();
  let rejected=false;try{await app.loadUSDFromData(new Uint8Array([1,2,3]),'bad.usda');}catch{rejected=true;}
  return {instances:instances[0],bounds:bounds[0],uploads,images,legacy:!!app.loader.native_.LightUSDLoaderNative,first,matrix:Array.from(mesh.worldMatrix),error:error?.message,rejected,cleared:app.nativeLoader===null};
 },Array.from(archive));
 assert.equal(result.images,1);assert.equal(result.uploads.length,1);
 assert.deepEqual(result.uploads[0].pixels,[255,0,0,255]);
 assert.match(result.uploads[0].format,/rgba8unorm/);
 assert.equal(result.legacy,false);assert.equal(result.first,1);assert.equal(result.error,undefined);
 assert.deepEqual(result.matrix.slice(12,15),[7,2,-3]);
 assert.deepEqual(result.instances[0].modelMatrix.slice(12,15),[7,2,-3]);
 assert.ok(Math.abs(result.instances[0].normalMatrix[5]-1/3)<1e-6);
 assert.deepEqual(result.bounds,{min:[5,-1,-7],max:[9,5,1]});assert.equal(result.rejected,true);assert.equal(result.cleared,true);
 assert.deepEqual(errors.filter(x=>!x.includes('Failed to load resource')&&!x.includes('Failed to load default scene')&&!x.includes('Failed to load environment map')),[]);
 console.log('PASS core OpenPBR WebGPU scene loading, transforms, repeated loads, invalid input and GPU validation');
}finally{await browser?.close();await server.close();}
