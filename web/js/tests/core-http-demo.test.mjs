// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createServer,preview} from 'vite';
import puppeteer from 'puppeteer';
process.env.LIGHTUSD_SKIP_WASM_PREPARE='1';
const production=process.argv.includes('--production');
const config={root:new URL('..',import.meta.url).pathname,server:{host:'127.0.0.1',port:0},preview:{host:'127.0.0.1',port:0}};
const server=production?await preview(config):await createServer(config);
if(!production)await server.listen();
let browser;
try {
 browser=await puppeteer.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage','--enable-unsafe-swiftshader']});
 const page=await browser.newPage();
 await page.setRequestInterception(true);
 const layer=name=>`#usda 1.0\ndef Scope "${name}" { asset texture=@same.png@ }`;
 page.on('request',req=>{
  const path=new URL(req.url()).pathname;
  if(path==='/http-fixture/a/ref.usda')return req.respond({status:200,body:layer('A')});
  if(path==='/http-fixture/b/ref.usda')return req.respond({status:200,body:layer('B')});
  if(path==='/http-fixture/a/same.png')return req.respond({status:200,body:'texture-a'});
  if(path==='/http-fixture/b/same.png')return req.respond({status:200,body:'texture-b'});
  if(path.startsWith('/http-fixture/'))return req.respond({status:404,body:'missing'});
  req.continue();
 });
 await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/http-asset-resolver.html`);
 await page.waitForFunction(()=>!!window.httpAssetDemo,{timeout:60000});
 const output=await page.evaluate(async()=>{
  const {flattenNextOverHttp,HttpAssetResolver,renderer}=window.httpAssetDemo;
  const encode=x=>new TextEncoder().encode(x);
  const baseUrl=location.origin+'/http-fixture/';
  const rootBytes=encode('#usda 1.0\n( subLayers=[@a/ref.usda@,@b/ref.usda@] )');
  const resolver=new HttpAssetResolver({baseUrl});
  const result=await flattenNextOverHttp({renderer,rootBytes,filename:'root.usda',resolver});
  const entries=window.httpAssetDemo.parseUSDZEntries(result.usdz);
  const doc=new renderer.native.LayerDocument();
  let text;
  try {if(!doc.load(entries[0].data).success)throw Error(doc.error());text=doc.exportUSDA().text;}finally{doc.delete();}
  const preload=new HttpAssetResolver({baseUrl});
  const cached=await flattenNextOverHttp({renderer,rootBytes,filename:'root.usda',resolver:preload,preloadedAssets:[
   {key:'a/ref.usda',bytes:encode('#usda 1.0\ndef Scope "A" { asset texture=@same.png@ }')},
   {key:'b/ref.usda',bytes:encode('#usda 1.0\ndef Scope "B" { asset texture=@same.png@ }')},
   {key:'a/same.png',bytes:encode('texture-a')},{key:'b/same.png',bytes:encode('texture-b')}
  ]});
  let failed=false;
  try{await flattenNextOverHttp({renderer,rootBytes:encode('#usda 1.0\n( subLayers=[@missing.usda@] )'),filename:'root.usda',resolver});}catch{failed=true;}
  return {text,assets:entries.slice(1).map(x=>({name:x.name,data:new TextDecoder().decode(x.data)})),fetches:resolver.fetchLog.filter(x=>x.ok).map(x=>x.url),cachedFetches:preload.fetchLog.length,cachedAssets:cached.packagedAssets,failed};
 });
 assert.equal(output.assets.length,2);assert.deepEqual(output.assets.map(x=>x.data).sort(),['texture-a','texture-b']);
 assert.equal(new Set(output.assets.map(x=>x.name)).size,2);
 assert.equal(output.cachedFetches,0);assert.equal(output.cachedAssets,2);assert.equal(output.failed,true);
 assert.equal(output.fetches.length,4);
 assert.ok(JSON.stringify(output.text).includes('assets/0.png'));assert.ok(JSON.stringify(output.text).includes('assets/1.png'));
 console.log('PASS core HTTP composition: layer anchors, distinct textures, safe packaging, preloaded assets, missing dependencies');
} finally {await browser?.close();await server.close();}
