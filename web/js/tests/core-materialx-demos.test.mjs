// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import puppeteer from 'puppeteer';
import {PNG} from 'pngjs';
import {buildUSDZWithNewRoot} from '../src/usdzconvert.js';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const fixture=`#usda 1.0
(defaultPrim="World"; upAxis="Z")
def Xform "World" {
 def Mesh "Quad" {
  uniform token subdivisionScheme="none"
  point3f[] points=[(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
  int[] faceVertexCounts=[4]
  int[] faceVertexIndices=[0,1,2,3]
  texCoord2f[] primvars:st=[(0,0),(1,0),(1,1),(0,1)] (interpolation="vertex")
  rel material:binding=</World/Look>
 }
 def Material "Look" {
  token outputs:surface.connect=</World/Look/Surface.outputs:surface>
  def Shader "Surface" {
   uniform token info:id="ND_open_pbr_surface_surfaceshader"
   color3f inputs:base_color.connect=</World/Look/Texture.outputs:rgb>
   float inputs:base_weight=1
   float inputs:specular_roughness=0.3
   token outputs:surface
  }
  def Shader "Texture" {
   uniform token info:id="UsdUVTexture"
   asset inputs:file=@textures/check.png@
   float2 inputs:st.connect=</World/Look/UV.outputs:result>
   float3 outputs:rgb
  }
  def Shader "UV" {
   uniform token info:id="UsdPrimvarReader_float2"
   token inputs:varname="st"
   float2 outputs:result
  }
 }
}`;
const png=PNG.sync.write({width:2,height:2,data:Buffer.from([
  255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255])});
const archive=buildUSDZWithNewRoot('scene.usda',new TextEncoder().encode(fixture),[
  {name:'textures/check.png',data:png}]);
// PMREM needs a real image extent; a 1x1 HDR yields a zero-sized cube and
// makes textured/untextured surfaces equally black, defeating the visual oracle.
const hdrRows=[];
for(let row=0;row<32;row++)hdrRows.push(Buffer.from([2,2,0,64,192,128,192,128,192,128,192,129]));
const hdr=Buffer.concat([Buffer.from('#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 32 +X 64\n'),...hdrRows]);
const config={root:new URL('..',import.meta.url).pathname,
  configFile:new URL('../vite.config.ts',import.meta.url).pathname,
  server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);
if(!production)await server.listen();
let browser;
try {
  browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage',
    '--enable-unsafe-webgpu','--enable-unsafe-swiftshader',
    ...(process.platform==='linux'?['--enable-features=Vulkan','--use-angle=vulkan',
      '--use-vulkan=swiftshader','--use-webgpu-adapter=swiftshader','--disable-vulkan-surface']:['--use-angle=swiftshader'])]});
  const page=await browser.newPage();
  const errors=[];let phase='debug';
  page.on('pageerror',error=>errors.push(phase+': '+(error.stack||error.message)));
  page.on('console',message=>{if(message.type()==='error')errors.push(phase+': '+message.text().slice(0,2000));});
  await page.setRequestInterception(true);
  page.on('request',request=>{
    if(request.url().endsWith('/fancy-teapot-mtlx.usdz'))request.respond({status:200,body:Buffer.from(archive),contentType:'application/octet-stream'}).catch(()=>{});
    else if(/\.hdr(?:\?|$)/.test(request.url()))request.respond({status:200,body:hdr,contentType:'application/octet-stream'}).catch(()=>{});
    else request.continue().catch(()=>{});
  });
  const base=`http://127.0.0.1:${server.httpServer.address().port}`;
  await page.goto(base+'/mtlx-debug.html');
  await page.waitForFunction(()=>window._debugMtlx?.nativeLoader?.numMeshes()===1,{timeout:60000});
  await page.waitForFunction(()=>document.querySelector('#object-count')?.textContent==='1' && document.querySelector('#material-count')?.textContent==='1',{timeout:30000});
  const debug=await page.evaluate(()=>{
    const state=window._debugMtlx, native=state.nativeLoader;
    return {meshes:native.numMeshes(),materials:state.materials.length,images:native.numImages(),
      map:!!state.materials[0]?.threeMaterial?.map,metadata:native.getSceneMetadata(),
      pixels:native.getImageCopy(0),rotation:state.root.rotation.x};
  });
  assert.equal(debug.meshes,1);assert.equal(debug.materials,1);assert.equal(debug.images,1);
  assert.equal(debug.map,true,JSON.stringify(debug));
  assert.equal(debug.metadata.upAxis,'Z');
  assert.ok(Math.abs(debug.rotation+Math.PI/2)<1e-6);
  phase='gpu-load';
  await page.goto(base+'/materialx-webgpu.html');
  await page.waitForFunction(()=>/Loaded:|not supported|Initialization failed|Failed to parse/.test(document.querySelector('#status')?.textContent||''),{timeout:60000});
  const status=await page.$eval('#status',element=>element.textContent);
  assert.match(status,/Loaded: 1 meshes, 1 materials/,status+'\n'+errors.join('\n'));
  phase='gpu-first-render';
  const gpu=await page.evaluate(async()=>{
    const {loaderState,sceneState,threeState}=window._debugMaterialXWebGPU;
    await window._debugMaterialXWebGPU.renderFrame();
    return {meshes:loaderState.nativeLoader.numMeshes(),images:loaderState.nativeLoader.numImages(),
      materials:sceneState.materials.length,map:!!sceneState.materials[0]?.map,
      metadata:sceneState.metadata};
  });
  assert.equal(gpu.meshes,1);assert.equal(gpu.images,1);assert.equal(gpu.materials,1);
  assert.equal(gpu.map,true,JSON.stringify(gpu));assert.equal(gpu.metadata.upAxis,'Z');
  for(const mode of ['physical','openpbr-tsl']) {
    phase='gpu-'+mode;
    await page.evaluate(mode=>{
      const row=[...document.querySelectorAll('.controller')].find(element=>element.querySelector('.name')?.textContent==='Shader Mode');
      const select=row?.querySelector('select');if(!select)throw Error('Missing shader mode selector');
      select.value=mode;select.dispatchEvent(new Event('change',{bubbles:true}));
    },mode);
    await page.waitForFunction(()=>document.querySelector('#status')?.textContent==='Materials reloaded',{timeout:30000});
    assert.equal(await page.evaluate(async()=>{
      const {sceneState,threeState}=window._debugMaterialXWebGPU;
      await window._debugMaterialXWebGPU.renderFrame();
      return !!sceneState.materials[0]?.map;
    }),true);
    const changed=await page.evaluate(async()=>{
      const debug=window._debugMaterialXWebGPU;
      const material=debug.sceneState.materials[0], texture=material.map;
      const mapped=await debug.capturePixels();
      try {
        material.map=null;material.needsUpdate=true;
        const plain=await debug.capturePixels();
        let mesh;
        debug.sceneState.root.traverse(object=>{if(object.isMesh)mesh=object;});
        return {changed:mapped.reduce((count,value,index)=>count+(value!==plain[index]?1:0),0),
          bytes:mapped.length,unique:new Set(mapped).size,color:material.color?.toArray(),
          uv:mesh?.geometry.attributes.uv?.count,position:mesh?.geometry.attributes.position?.count,
          index:mesh?.geometry.index?.count,visible:mesh?.visible,side:material.side,
          material:material.type,rootMatrix:debug.sceneState.root.matrixAutoUpdate,
          values:[...new Set(mapped)].slice(0,12),pixelType:mapped.constructor.name,
          matrix:mesh?.matrixWorld.toArray(),camera:debug.threeState.camera.position.toArray(),
          cameraMatrix:debug.threeState.camera.matrixWorldInverse.toArray(),
          projection:debug.threeState.camera.projectionMatrix.toArray(),
          groups:mesh?.geometry.groups,drawRange:mesh?.geometry.drawRange,
          sceneParent:debug.sceneState.root.parent===debug.threeState.scene,
          materialMatch:Array.isArray(mesh?.material)?mesh.material.includes(material):mesh?.material===material};
      } finally {material.map=texture;material.needsUpdate=true;}
    });
    assert.ok(changed.uv>0,'Missing core UV attributes');
    assert.ok(changed.changed>32,mode+' did not sample the packaged base-color texture: '+JSON.stringify(changed)+' '+errors.join(' | '));
  }
  const deviceState=await page.evaluate(async()=>{
    const adapter=await navigator.gpu.requestAdapter();
    const info=adapter.info || await adapter.requestAdapterInfo();
    const device=window._debugMaterialXWebGPU.threeState.renderer.backend.device;
    const lost=await Promise.race([device.lost.then(info=>info.reason),
      new Promise(resolve=>setTimeout(()=>resolve(false),100))]);
    return {adapter:[info.vendor,info.architecture,info.device,info.description].join(' '),lost};
  });
  if(process.platform==='linux')assert.match(deviceState.adapter,/swiftshader/i);
  assert.equal(deviceState.lost,false);
  assert.deepEqual(errors,[]);
  console.log('PASS: '+(production?'production':'development')+' MaterialX debug/WebGPU core streams, USDZ textures, metadata and rendering');
} finally {
  if(browser)await browser.close();
  await server.close();
}
