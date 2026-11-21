// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import puppeteer from 'puppeteer';
import {PNG} from 'pngjs';
import {buildUSDZWithNewRoot} from '../src/usdzconvert.js';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const config={root:new URL('..',import.meta.url).pathname,configFile:new URL('../vite.config.ts',import.meta.url).pathname,
 server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);if(!production)await server.listen();
const cube=new TextEncoder().encode(`#usda 1.0
def Cube "Box" {
 double3 xformOp:translate=(7,2,-3)
 double3 xformOp:scale=(2,3,4)
 uniform token[] xformOpOrder=["xformOp:translate","xformOp:scale"]
}`);
const source=new TextEncoder().encode(`#usda 1.0
def Mesh "Quad" {
 uniform token subdivisionScheme="none"
 point3f[] points=[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
 int[] faceVertexCounts=[4]
 int[] faceVertexIndices=[0,1,2,3]
 texCoord2f[] primvars:st=[(0,0),(1,0),(1,1),(0,1)] (interpolation="vertex")
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
  asset inputs:file=@textures/check.png@
  float3 outputs:rgb
 }
}`);
const png=color=>PNG.sync.write({width:1,height:1,data:Buffer.from(color)});
const archive=buildUSDZWithNewRoot('nested/root.usda',source,[
 {name:'nested/textures/check.png',data:png([255,0,0,255])},
 {name:'textures/check.png',data:png([0,255,0,255])}]);
let browser;
try {
 browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader']});
 const page=await browser.newPage(),errors=[];page.on('pageerror',error=>errors.push(error.message));
 await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/streaming.html`);
 await page.waitForFunction(()=>!!window.streamingRenderer,{timeout:60000});
 const results=await page.evaluate(async({cube,archive})=>{
  const renderer=window.streamingRenderer;
  if(renderer.native.LightUSDLoaderNative)throw Error('Expected core-only module');
  const output=[];
  let created=0,deleted=0;
  const Original=renderer.native.RenderStream;
  renderer.native.RenderStream=function(...args){
   const stream=new Original(...args);created++;
   return new Proxy(stream,{get(target,key){
    if(key==='delete')return ()=>{deleted++;return target.delete();};
    const value=Reflect.get(target,key);return typeof value==='function'?value.bind(target):value;
   }});
  };
  for(const method of ['loadBytes','loadBytesIncremental']) {
   const padded=new Uint8Array(cube.length+8);padded.set(cube,4);
   const stats=await renderer[method](new DataView(padded.buffer,4,cube.length),'cube.usda');
   renderer._render();
   const normal=[...renderer.gl.getUniform(renderer.prog,renderer.loc.uNormalMat)];
   const bounds=structuredClone(renderer.bbox),model=[...renderer.drawables[0].model];
   const buffers=[...renderer._glBuffers];
   const textured=await renderer[method](new Uint8Array(archive),'textured.usdz');
   const released=buffers.every(buffer=>!renderer.gl.isBuffer(buffer));
   renderer._render();
   const gl=renderer.gl,pixels=new Uint8Array(gl.drawingBufferWidth*gl.drawingBufferHeight*4);
   gl.readPixels(0,0,gl.drawingBufferWidth,gl.drawingBufferHeight,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
   let red=0,green=0;for(let i=0;i<pixels.length;i+=4){if(pixels[i]>pixels[i+1]+30)red++;if(pixels[i+1]>pixels[i]+30)green++;}
   output.push({method,stats:stats.meshes,bounds,model,normal,textures:textured.textures,
    mapped:!!renderer.drawables[0].material.baseMap,released,red,green,error:gl.getError()});
  }
  for(const method of ['loadBytes','loadBytesIncremental']) {
   let failed=false;
   try {await renderer[method](new Uint8Array([80,75,3,4]),'broken.usdz');}catch{failed=true;}
   if(!failed)throw Error('Malformed package accepted');
  }
  renderer.dispose();
  if(created!==deleted)throw Error(`Native handle leak: ${created} created, ${deleted} deleted`);
  renderer.native.RenderStream=Original;
  return output;
 },{cube:[...cube],archive:[...archive]});
 for(const result of results) {
  assert.equal(result.stats,1);assert.deepEqual(result.bounds,{min:[5,-1,-7],max:[9,5,1]});
  assert.deepEqual(result.model.slice(12,15),[7,2,-3]);
  for(const [index,value] of [[0,0.5],[4,1/3],[8,0.25]])assert.ok(Math.abs(result.normal[index]-value)<1e-6);assert.equal(result.released,true);
  assert.equal(result.mapped,true,JSON.stringify(result));assert.ok(result.textures>0);
  assert.ok(result.red>32 && result.green<result.red/10,JSON.stringify(result));assert.equal(result.error,0);
 }
 assert.deepEqual(errors,[]);
 console.log('PASS: '+(production?'production':'development')+' core streaming/standard loads, USDA, nested USDZ texture pixels, world transforms and GPU buffer cleanup');
} finally {await browser?.close();if(server.close)await server.close();else await new Promise(resolve=>server.httpServer.close(resolve));}
