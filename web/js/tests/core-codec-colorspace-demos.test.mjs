// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {fileURLToPath} from 'node:url';
import {createServer, preview} from 'vite';
import puppeteer from 'puppeteer';
process.env.LIGHTUSD_SKIP_WASM_PREPARE = '1';
const root = fileURLToPath(new URL('..', import.meta.url));
const production = process.argv.includes('--production');
const memory64 = production && fs.existsSync(root + '/src/lightusd/lightusd_64.js');
if (production) {
  if(memory64)for(const name of ['lightusd','lightusd_geometry','lightusd_raytrace','lightusd_validator','lightusd_exr','lightusd_mcp']) {
    if(name==='lightusd_mcp' && !fs.existsSync(root+'/src/lightusd/'+name+'_64.js'))continue;
    for(const extension of ['js','wasm'])assert.ok(fs.existsSync(root+'/dist/assets/'+name+'_64.'+extension),`Missing memory64 production artifact: ${name}.${extension}`);
  }
  for (const name of fs.readdirSync(root).filter(name => name.endsWith('.html') && name !== 'validation-body.html'))
    assert.ok(fs.existsSync(root + '/dist/' + name), `Missing production demo: ${name}`);
}
const config = {root, server:{host:'127.0.0.1',port:0}, preview:{host:'127.0.0.1',port:0}};
const server = production ? await preview(config) : await createServer(config);
if (!production) await server.listen();
let browser;
try {
  browser = await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader']});
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  const base = `http://127.0.0.1:${server.httpServer.address().port}`;
  await page.goto(base + '/colorspace-regression.html');
  await page.waitForFunction(() => document.documentElement.dataset.regressionReady === 'true', {timeout:60000});
  const result = await page.evaluate(() => window.__colorRegression);
  assert.equal(result.pass, true, JSON.stringify(result));
  assert.ok(result.backendPixels.adapter && result.backendPixels.stream);
  await page.goto(base + '/texcomp.html');
  await page.waitForFunction(() => /GPU upload:|ERROR:/.test(document.querySelector('#log').textContent), {timeout:60000});
  const codec = await page.evaluate(() => ({log:document.querySelector('#log').textContent, bytes:document.querySelector('#gpu').textContent}));
  assert.doesNotMatch(codec.log, /ERROR:|compressed path failed/);
  assert.match(codec.bytes, /KiB/);
  if(memory64) {
    const result=await page.evaluate(async()=>{
      if(!WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0,5,3,1,4,1])))return {supported:false};
      const {default:factory}=await import('/assets/lightusd_64.js');
      const native=await factory(),document=new native.LayerDocument();
      try {
        const loaded=document.load(new TextEncoder().encode('#usda 1.0\ndef Xform "Memory64" {}\n'));
        if(!loaded.success)throw Error(loaded.error);
        const geometry=await native.loadFeature('geometry');
        const validator=await native.loadFeature('validator');
        return {supported:true,bits:native.featureInfo.pointerBits,
          geometryBits:geometry.featureInfo.pointerBits,validatorBits:validator.featureInfo.pointerBits,
          separate:geometry.HEAPU8.buffer!==native.HEAPU8.buffer && validator.HEAPU8.buffer!==native.HEAPU8.buffer,
          text:document.exportUSDA().text};
      }finally{document.delete();}
    });
    if(result.supported) {
      assert.equal(result.bits,64);assert.equal(result.geometryBits,64);assert.equal(result.validatorBits,64);
      assert.equal(result.separate,true);assert.match(result.text,/Memory64/);
      console.log('PASS production memory64 core and independent feature loading');
    }else console.log('SKIP memory64 runtime: browser lacks memory64 support; all artifact pairs verified');
  }
  assert.deepEqual(errors, []);
  console.log(`PASS ${production?'production':'development'} codec and core colorspace demos${production?', all demo HTML entries present':''}`);
} finally {await browser?.close(); await server.close();}
