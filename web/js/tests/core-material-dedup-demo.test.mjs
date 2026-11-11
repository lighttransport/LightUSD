// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import puppeteer from 'puppeteer';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const config={root:new URL('..',import.meta.url).pathname,server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);if(!production)await server.listen();
let browser;
try {
 browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader']});
 const page=await browser.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
 const output=[];
 for(const worker of [0,1]) {
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/material-dedup.html?worker=${worker}&textures=0`);
  await page.waitForFunction(()=>window.renderComplete||window.renderError,{timeout:90000});
  const result=await page.evaluate(()=>{
   const d=window.__materialDedupDebug;
   return {error:window.renderError,backend:d?.params.backend,counts:d?.currentCounts,sceneBackend:d?.currentUsd?.__backend,
    objects:d?.root?.children.length,status:document.querySelector('#status').textContent};
  });
  assert.equal(result.error,null,JSON.stringify(result));assert.equal(result.backend,'core');
  assert.equal(result.sceneBackend,'core');assert.ok(result.counts.meshes>0,JSON.stringify(result));assert.ok(result.objects>0);
  output.push(result.counts);
 }
 assert.equal(output[0].meshes,output[1].meshes);assert.equal(output[0].materials,output[1].materials);
 assert.deepEqual(errors,[]);
 console.log('PASS core material-dedup demo main-thread/worker geometry and material counts');
}finally{await browser?.close();await server.close();}
