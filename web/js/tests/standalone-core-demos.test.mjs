// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import fs from 'node:fs';
import {unpackUSDZ} from '../src/usdzconvert.js';
import puppeteer from 'puppeteer';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const config={root:new URL('..',import.meta.url).pathname,
  configFile:new URL('../vite.config.ts',import.meta.url).pathname,
  server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);
if(!production)await server.listen();
let browser;
try {
  browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader']});
  const page=await browser.newPage(),errors=[],consoleErrors=[];
  page.on('console',message=>{if(message.type()==='error')consoleErrors.push(message.text().slice(0,5000));});
  page.on('pageerror',error=>errors.push(error.stack || error.message));
  const base=`http://127.0.0.1:${server.httpServer.address().port}`;
  await page.evaluateOnNewDocument(()=>{
    window.captured=[];
    const create=URL.createObjectURL.bind(URL);
    URL.createObjectURL=blob=>{window.captured.push(blob);return create(blob);};
    HTMLAnchorElement.prototype.click=function() {};
  });
  if(!process.argv.includes('--physics-only')) {
  await page.goto(base+'/usd-export.html');
  await page.click('#btnGenerate');
  await page.waitForFunction(()=>document.querySelector('#log')?.textContent.includes('Sample scene created'),{timeout:45000});
  for(const id of ['btnUSDA','btnUSDC','btnUSDZ'])await page.click('#'+id);
  await page.waitForFunction(()=>window.captured.length===3);
  const downloaded=await page.evaluate(async()=>Promise.all(window.captured.map(async blob=>[...new Uint8Array(await blob.arrayBuffer())])));
  assert.match(new TextDecoder().decode(new Uint8Array(downloaded[0])),/@textures\/checkerboard.png@/);
  assert.equal(new TextDecoder().decode(new Uint8Array(downloaded[1]).subarray(0,8)),'PXR-USDC');
  const archive=unpackUSDZ(new Uint8Array(downloaded[2]));
  assert.equal(archive.order[0],'root.usdc');
  assert.deepEqual([...archive.entries.get('textures/checkerboard.png').subarray(0,8)],[137,80,78,71,13,10,26,10]);
  await page.goto(base+'/validation.html?backend=legacy');
  await page.click('#loadSample');
  await page.waitForFunction(()=>!document.querySelector('#validate').disabled,{timeout:45000});
  await page.click('#validate');
  await page.waitForFunction(()=>/^(Passed|Failed|Incomplete|Execution failed)/.test(document.querySelector('#summaryStatus')?.textContent||''),{timeout:45000});
  assert.match(await page.$eval('#summaryStatus',element=>element.textContent),/^Passed/);
  assert.equal(await page.$('#backend'),null);
  {
    const fixture=`#usda 1.0
 def Mesh "Triangle" {
  uniform token subdivisionScheme="none"
  point3f[] points=[(0,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts=[3]
  int[] faceVertexIndices=[0,1,2]
  rel material:binding=</Look>
 }
 def Material "Look" {
  token outputs:surface.connect=</Look/Shader.outputs:surface>
  def Shader "Shader" {
   uniform token info:id="ND_open_pbr_surface_surfaceshader"
   color3f inputs:base_color=(0.3,0.5,0.7)
   float inputs:specular_roughness=0.2
   float inputs:coat_weight=0.4
   token outputs:surface
  }
 }`;
    const fixtureURL=base+'/_migration-materialx.usda';
    const respond=request=>{
      if(request.url()===fixtureURL)request.respond({status:200,contentType:'application/octet-stream',body:fixture}).catch(()=>{});
      else request.continue().catch(()=>{});
    };
    await page.setRequestInterception(true);page.on('request',respond);
    await page.goto(base+'/materialx.html?backend=legacy&skipEnvironment=1&skipDomeLight=1&uri='+encodeURIComponent(fixtureURL));
    await page.waitForFunction(()=>window.renderComplete===true || document.querySelector('#status')?.textContent.startsWith('Error:'),{timeout:45000}).catch(async error=>{
      const state=await page.evaluate(()=>({status:document.querySelector('#status')?.textContent,renderError:window.renderError,meshes:document.querySelector('#mesh-count')?.textContent}));
      throw new Error(error.message+' '+JSON.stringify({state,errors}));
    });
    const status=await page.$eval('#status',element=>element.textContent);
    assert.ok(!status.startsWith('Error:'),status+' '+consoleErrors.join('\n'));
    assert.equal(await page.$eval('#mesh-count',element=>element.textContent),'1');
    assert.match(await page.$eval('#status',element=>element.textContent),/backend: next/);
    const edited=await page.evaluate(()=>{
      const material=window._debugMaterialX.sceneState.materials[0];
      const parameter=material.userData.rawData.openPBR.coat_ior;
      parameter.sourceColorSpace='raw';
      const row=[...document.querySelectorAll('.controller')].find(element=>element.querySelector('.name')?.textContent==='Coat IOR');
      const input=row?.querySelector('input[type=number]');
      if(!input)throw Error('Missing core Coat IOR editor');
      input.value='2.2';input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));
      const updated=material.userData.rawData.openPBR.coat_ior;
      return {value:Array.isArray(updated.value)?updated.value[0]:updated.value,metadata:updated.sourceColorSpace,
        core:typeof window._debugMaterialX.loaderState.loader.native_.RenderStream==='function',
        legacy:typeof window._debugMaterialX.loaderState.loader.native_.LightUSDLoaderNative};
    });
    assert.deepEqual(edited,{value:2.2,metadata:'raw',core:true,legacy:'undefined'});
    await page.evaluate(()=>{
      const row=[...document.querySelectorAll('.controller')].find(element=>element.querySelector('.name')?.textContent==='Implementation');
      const select=row?.querySelector('select');if(!select)throw Error('Missing material implementation selector');
      select.value='openpbr';select.dispatchEvent(new Event('change',{bubbles:true}));
    });
    await page.waitForFunction(()=>window._debugMaterialX.sceneState.materials[0]?.isOpenPBRMaterial===true,{timeout:15000});
    const liveCoatIOR=await page.evaluate(()=>{
      const row=[...document.querySelectorAll('.controller')].find(element=>element.querySelector('.name')?.textContent==='Coat IOR');
      const input=row?.querySelector('input[type=number]');
      if(!input)throw Error('Missing OpenPBR Coat IOR editor');
      input.value='2.6';input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));
      return window._debugMaterialX.sceneState.materials[0].uniforms.coat_ior.value;
    });
    assert.equal(liveCoatIOR,2.6);
    await page.evaluate(async()=>{
      const {threeState}=window._debugMaterialX;
      await threeState.renderer.compileAsync(threeState.scene,threeState.camera);
      threeState.renderer.render(threeState.scene,threeState.camera);
      await new Promise(requestAnimationFrame);
    });
    assert.ok(!consoleErrors.some(message=>message.includes('Shader Error')),consoleErrors.join('\n'));

    page.off('request',respond);await page.setRequestInterception(false);
  }
  await page.goto(base+'/udim.html');
  await page.click('#btnSample');
  await page.waitForFunction(()=>/Loaded|ERROR:/.test(document.querySelector('#log')?.textContent||''),{timeout:60000});
  assert.match(await page.$eval('#log',element=>element.textContent),/Loaded/);
  assert.match(await page.$eval('#info',element=>element.textContent),/1001/);
  await page.click('#btnExport');
  await page.waitForFunction(()=>window.captured.length===1);
  const sparse=await page.evaluate(async()=>[...new Uint8Array(await window.captured[0].arrayBuffer())]);
  const tiles=unpackUSDZ(new Uint8Array(sparse));
  assert.equal(tiles.order.filter(name=>name.endsWith('.png')).length,4);
  assert.match(new TextDecoder().decode(tiles.entries.get(tiles.order[0])),/<UDIM>/);
  await page.click('input[name=mode][value=combine]');
  await page.waitForFunction(()=>/Loaded \(combine\)|ERROR:/.test(document.querySelector('#log')?.textContent||''),{timeout:60000});
  assert.match(await page.$eval('#log',element=>element.textContent),/Loaded \(combine\)/);
  if(production) {
    const workerFile=fs.readdirSync(new URL('../dist/assets',import.meta.url)).find(name=>/^xatlas-worker-.*\.js$/.test(name));
    assert.ok(workerFile);
    const result=await page.evaluate(file=>new Promise((resolve,reject)=>{
      const worker=new Worker('/assets/'+file,{type:'module'});
      const timer=setTimeout(()=>{worker.terminate();reject(Error('xatlas worker timeout'));},30000);
      worker.onerror=event=>{clearTimeout(timer);worker.terminate();reject(Error(event.message));};
      worker.onmessage=({data})=>{clearTimeout(timer);worker.terminate();data.type==='error'?reject(Error(data.message)):resolve({vertices:data.result.vertexCount,width:data.result.width});};
      worker.postMessage({type:'unwrap',positions:new Float32Array([0,0,0,1,0,0,0,1,0]),indices:new Uint32Array([0,1,2]),options:{resolution:64}});
    }),workerFile);
    assert.equal(result.vertices,3);assert.ok(result.width>0);
    await page.goto(base+'/lucia-code/index.html');
    await page.waitForSelector('#scene-tree .tree-row',{timeout:45000});
  }
  }
  if(process.env.MUJOCO_WASM_DIR) {
    const respond=request=>{
      const match=new URL(request.url()).pathname.match(/\/assets\/mujoco\/(mujoco_physics\.(?:js|wasm))$/);
      if(match)request.respond({status:200,contentType:match[1].endsWith('.js')?'application/javascript':'application/wasm',
        body:fs.readFileSync(process.env.MUJOCO_WASM_DIR+'/'+match[1])}).catch(()=>{});
      else request.continue().catch(()=>{});
    };
    await page.setRequestInterception(true);page.on('request',respond);
    await page.goto(base+'/phys-sim.html');
    await page.waitForFunction(()=>document.querySelector('#status')?.textContent==='Simulating.',{timeout:60000}).catch(async error=>{
      throw Error(error.message+' '+JSON.stringify({status:await page.$eval('#status',element=>element.textContent),errors,consoleErrors}));
    });
    const physics=JSON.parse(await page.$eval('#physicsJson',element=>element.textContent));
    assert.ok(physics.prims.length>0);
    await page.waitForFunction(()=>parseFloat(document.querySelector('#simTime').textContent)>0.05,{timeout:15000});
    await page.click('#playPause');
    assert.equal(await page.$eval('#status',element=>element.textContent),'Paused.');
    await page.click('#reset');
    assert.equal(await page.$eval('#simTime',element=>element.textContent),'0.000 s');
    page.off('request',respond);await page.setRequestInterception(false);
  }
  assert.deepEqual(errors,[]);
  console.log(process.argv.includes('--physics-only')?'PASS: core physics browser extraction, real MuJoCo stepping, pause and reset':'PASS: '+(production?'production':'development')+' core sample/export, validator Worker, MaterialX GL, sparse/combined UDIM'+(production?', Lucia startup and xatlas Worker':''));
} finally {await browser?.close();if(server.close)await server.close();else await new Promise(resolve=>server.httpServer.close(resolve));}
