// SPDX-License-Identifier: Apache-2.0
// Build web/demo with Vite before running this production-bundle integration check.
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
import puppeteer from 'puppeteer';
import {buildUSDZWithNewRoot} from '../src/usdzconvert.js';
const root=fileURLToPath(new URL('../../../', import.meta.url)).replace(/\/$/, '');
const dist=root+'/web/demo/dist';
const types={'.html':'text/html','.js':'text/javascript','.wasm':'application/wasm','.json':'application/json','.png':'image/png','.jpg':'image/jpeg','.svg':'image/svg+xml'};
const server=http.createServer((req,res)=>{
 const file=path.resolve(dist,'.'+new URL(req.url,'http://localhost').pathname);
 if(!file.startsWith(dist+'/')||!fs.existsSync(file)||!fs.statSync(file).isFile()){res.writeHead(404);res.end();return;}
 res.setHeader('Content-Type',types[path.extname(file)]||'application/octet-stream');
 fs.createReadStream(file).pipe(res);
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
let browser;
try{
 browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader']});
 const page=await browser.newPage();
 page.on('pageerror',e=>console.error('PAGE:',e.message));
 page.on('console',m=>{if(m.type()==='error')console.error('CONSOLE:',m.text());});
 await page.goto(`http://127.0.0.1:${server.address().port}/backend-compare.html`);
 await page.waitForFunction(()=>document.querySelector('#roundtrip-stats')?.textContent.includes('meshes'),{timeout:45000});
 const workerFile=fs.readdirSync(dist+'/assets').find(name=>/^openchess-lightrt\.worker-.*\.js$/.test(name));
 const workerResult=await page.evaluate(file=>new Promise((resolve,reject)=>{
   const worker=new Worker('/assets/'+file,{type:'module'});
   const timer=setTimeout(()=>{worker.terminate();reject(Error('Worker timeout'));},30000);
   worker.onerror=e=>{clearTimeout(timer);worker.terminate();reject(Error(e.message));};
   worker.onmessage=({data})=>{clearTimeout(timer);worker.terminate();data.type==='error'?reject(Error(data.message)):resolve(data);};
   worker.postMessage({type:'build',generation:1,scene:{
     positions:new Float32Array([0,0,0,1,0,0,0,1,0]),normals:new Float32Array(9),
     colors:new Float32Array(9),vertexParams:new Float32Array(12),materialIds:new Int32Array([0]),materials:new Float32Array(10)}});
 }),workerFile);
 assert.deepEqual(workerResult,{type:'built',generation:1,triangles:1});
 const counts=await page.evaluate(()=>({source:document.querySelector('#source-stats').textContent,roundtrip:document.querySelector('#roundtrip-stats').textContent}));
 assert.match(counts.source, /[1-9]\d* meshes/);
 assert.equal(counts.roundtrip,counts.source);
 console.log('PASS: production core roundtrip demo and raytrace Worker');
 // Exercise every page using the shared production foundation, with small
 // offline USDA/USDC/USDZ inputs instead of downloading optional artist assets.
 const fixture = new TextEncoder().encode(`#usda 1.0
 def Mesh "Triangle" {
  uniform token subdivisionScheme="none"
  point3f[] points=[(-1,0,0),(1,0,0),(0,1,0)]
  int[] faceVertexCounts=[3]
  int[] faceVertexIndices=[0,1,2]
  color3f[] primvars:displayColor=[(0.8,0.2,0.1)]
 }
 `);
 const {default:createCore}=await import('../src/lightusd/lightusd.js');
 const native=await createCore();const document=new native.LayerDocument();let crate;
 try{assert.equal(document.load(fixture).success,true);const out=document.exportUSDC();assert.equal(out.success,true);crate=out.data;}finally{document.delete();}
 const archive=buildUSDZWithNewRoot('root.usdc',crate,[]);
 const entries=fs.readdirSync(root+'/web/demo/src/entries').filter(name=>name.endsWith('.js')&&fs.readFileSync(root+'/web/demo/src/entries/'+name,'utf8').includes('initDemo('));
 for(const entry of (process.argv.includes("--custom-only")?[]:entries)){
  const filename=entry.replace(/\.js$/,'.html');const demo=await browser.newPage();const errors=[];
  try{
   demo.on('pageerror',error=>errors.push(error.message));
   await demo.setRequestInterception(true);
   demo.on('request',request=>{
    const url=new URL(request.url());const extension=path.extname(url.pathname).toLowerCase();
    if(['.usd','.usda','.usdc','.usdz'].includes(extension))return request.respond({status:200,headers:{'access-control-allow-origin':'*'},contentType:'application/octet-stream',body:Buffer.from(extension==='.usdz'?archive:extension==='.usdc'?crate:fixture)}).catch(()=>{});
    if(url.origin!==`http://127.0.0.1:${server.address().port}`)return request.respond({status:404,body:'Offline migration fixture'}).catch(()=>{});
    request.continue().catch(()=>{});
   });
   await demo.goto(`http://127.0.0.1:${server.address().port}/${filename}`);
   await demo.waitForFunction(()=>{
    const app=window.__lightusdDemoApp;
    return app?.renderer&&(!app.config.defaultAsset||/^(Loaded|Failed:)/.test(app.statusEl?.textContent||''));
   },{timeout:60000});
   const state=await demo.evaluate(async()=>{
    const app=window.__lightusdDemoApp;
    if(!app.currentUsd)await app.loadURL('/migration-fixture.usda','migration fixture');
    await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
    const native=app.loader?.native_;
    return {status:app.statusEl.textContent,backend:app.currentUsd?.__backend,
      core:typeof native?.LayerDocument==='function'&&typeof native?.RenderStream==='function',
      legacy:typeof native?.LightUSDLoaderNative==='function',meshCount:app.currentUsd?.numMeshes?.(),
      canvas:app.renderer.domElement.width>0&&app.renderer.domElement.height>0};
   });
   assert.match(state.status,/^Loaded/,filename+': '+JSON.stringify(state));
   assert.equal(state.backend,'next',filename);assert.equal(state.core,true,filename);assert.equal(state.legacy,false,filename);
   assert.ok(state.meshCount>0,filename);assert.equal(state.canvas,true,filename);
   assert.deepEqual(errors,[],filename);
   console.log('PASS production core demo '+filename);
  }finally{await demo.close();}
 }
 if(!process.argv.includes("--custom-only")) console.log(`PASS: ${entries.length} production foundation entry points use core rendering`);
 const customPages=[
  ['animation-timeline','status',/Loaded|No animated|clips|frames/i],
  ['animation-blending','status',/Loaded|No animations|clips|blend/i],
  ['composition-viz','comp-status',/[1-9]\d* meshes/],
  ['streaming-viz','viz-status',/[1-9]\d* meshes/],
  ['viewer-toolkit','status',/Loaded/i],['usd-viewer','status',/Loaded/i],
  ['procedural-usd','editor-status',/Built: [1-9]\d* meshes/],
  ['usdz-packager','pack-status',/Loaded/i],['usd-diff','diff-status',/Comparing/],
  ['usd-inspector','status',/Loaded/i],['material-editor','status',/Loaded/i],
  ['usd-assets','asset-status',/[1-9]\d* meshes/],['usd-physics','status',/^Simulating/]
 ];
 for(const [name,statusId,ready]of customPages){
  const filter=process.argv.find(arg=>arg.startsWith('--page='))?.slice(7);if(filter&&filter!==name)continue;
  const demo=await browser.newPage(),errors=[];
  try{
   demo.on('pageerror',error=>errors.push(error.message));
   await demo.evaluateOnNewDocument(()=>{
    window.migrationDownloads=[];const create=URL.createObjectURL.bind(URL);
    URL.createObjectURL=blob=>{window.migrationDownloads.push(blob);return create(blob);};
    HTMLAnchorElement.prototype.click=function(){};
   });
   await demo.setRequestInterception(true);
   demo.on('request',request=>{
    const url=new URL(request.url()),extension=path.extname(url.pathname).toLowerCase();
    if(name!=='usd-physics'&&['.usd','.usda','.usdc','.usdz'].includes(extension))return request.respond({status:200,headers:{'access-control-allow-origin':'*'},contentType:'application/octet-stream',body:Buffer.from(extension==='.usdz'?archive:extension==='.usdc'?crate:fixture)}).catch(()=>{});
    if(url.origin!==`http://127.0.0.1:${server.address().port}`)return request.respond({status:404,body:'Offline migration fixture'}).catch(()=>{});
    request.continue().catch(()=>{});
   });
   await demo.goto(`http://127.0.0.1:${server.address().port}/${name}.html`);
   if(name==='usd-viewer'){
    await demo.waitForSelector('#url-input');await demo.$eval('#url-input',element=>{element.value=location.origin+'/migration-fixture.usda';});await demo.click('#url-btn');
   }else if(name==='usd-assets'){
    await demo.waitForFunction(()=>typeof window.__usdAssetsBrowser?.renderer?.native?.RenderStream==='function');
    await demo.evaluate(()=>new Promise(resolve=>requestAnimationFrame(resolve)));
    await demo.waitForSelector('#file-input');
    await demo.$eval('#file-input',(element,text)=>{const transfer=new DataTransfer();transfer.items.add(new File([text],'fixture.usda'));element.files=transfer.files;element.dispatchEvent(new Event('change',{bubbles:true}));},new TextDecoder().decode(fixture));
   }
   await demo.waitForFunction((id,pattern)=>{
    const status=document.getElementById(id)?.textContent||'';
    return new RegExp(pattern,'i').test(status)||/^(Error|Failed|Export failed)/.test(status);
   },{timeout:60000},statusId,ready.source).catch(async error=>{throw new Error(name+': '+await demo.$eval('#'+statusId,element=>element.textContent)+' '+error.message+' '+errors.join(';'));});
   const status=await demo.$eval('#'+statusId,element=>element.textContent);assert.match(status,ready,name);
   if(name==='usdz-packager'){
    for(const format of ['usda','usdc','usdz']){await demo.click('#export-'+format);await demo.waitForFunction(format=>document.getElementById('pack-status')?.textContent.startsWith('Exported '+format.toUpperCase()),{timeout:30000},format);}
    const exports=await demo.evaluate(async()=>Promise.all(window.migrationDownloads.slice(-3).map(async blob=>Array.from(new Uint8Array(await blob.arrayBuffer()).subarray(0,8)))));
    assert.equal(new TextDecoder().decode(new Uint8Array(exports[1])),'PXR-USDC');assert.deepEqual(exports[2].slice(0,4),[80,75,3,4]);
   }
   assert.ok(await demo.$('canvas'),name);assert.deepEqual(errors,[],name);console.log('PASS production custom demo '+name+'.html');
  }finally{await demo.close();}
 }


 if(!process.argv.some(arg=>arg.startsWith('--page='))){
  const validation=await browser.newPage();
  try{
   await validation.goto(`http://127.0.0.1:${server.address().port}/validation.html`);
   await validation.waitForSelector('#loadSample');await validation.click('#loadSample');
   await validation.waitForFunction(()=>!document.getElementById('validate').disabled,{timeout:60000});
   await validation.click('#validate');
   await validation.waitForFunction(()=>/^(Passed|Failed|Incomplete|Execution failed)/.test(document.getElementById('summaryStatus')?.textContent||''),{timeout:60000});
   assert.match(await validation.$eval('#summaryStatus',element=>element.textContent),/^Passed/);
   console.log('PASS production core validation demo');
  }finally{await validation.close();}
  const landing=await browser.newPage();
  try{
   await landing.goto(`http://127.0.0.1:${server.address().port}/index.html`);
   await landing.waitForFunction(()=>document.querySelectorAll('.demo-link').length===27);
   const links=await landing.$$eval('.demo-link',elements=>elements.map(element=>new URL(element.href).pathname.slice(1)).sort());
   const html=fs.readdirSync(dist).filter(name=>name.endsWith('.html')&&name!=='index.html').sort();
   assert.deepEqual(links,html);console.log('PASS production landing page links all 27 demo entries');
  }finally{await landing.close();}
 }
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
