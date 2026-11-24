// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {LightUSDLoader} from '../src/lightusd/LightUSDLoader.js';
const loader=new LightUSDLoader();await loader.init({useMemory64:process.env.LIGHTUSD_WASM64==='1'});
const native=loader.native_,Original=native.RenderStream;
let created=0,deleted=0;
native.RenderStream=function(){created++;const stream=new Original();return new Proxy(stream,{get(target,key){if(key==='delete')return()=>{deleted++;return target.delete();};const value=target[key];return typeof value==='function'?value.bind(target):value;}});};
const bytes=new TextEncoder().encode('#usda 1.0\ndef Cube "Box" {}');
const fetchOriginal=globalThis.fetch;
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'core-stream-'));
const response=(declared,body=bytes)=>new Response(new ReadableStream({start(c){c.enqueue(body.slice(0,5));c.enqueue(body.slice(5));c.close();}}),{headers:declared===null?{}:{'content-length':String(declared)}});
try {
 for(const declared of [null,bytes.length]) {
  globalThis.fetch=async()=>response(declared);
  let progress=0;
  const result=await loader.streamFetchToWasm('https://example.test/root.usda','root.usda',{onProgress:n=>{assert.ok(n>=progress);progress=n;}});
  try{assert.equal(result.bytesTransferred,bytes.length);assert.equal(result.usdInstance.beginStreamedAsset('root.usda').success,true);assert.equal(result.usdInstance.numMeshes(),1);}finally{result.usdInstance.delete();}
 }
 for(const declared of [bytes.length-1,bytes.length+1,2**30+1]) {
  globalThis.fetch=async()=>response(declared);
  await assert.rejects(loader.streamFetchToWasm('test','root.usda'));
  assert.equal(created,deleted);
 }
 globalThis.fetch=async()=>response(null);
 const cancelled=new AbortController();
 await assert.rejects(loader.streamFetchToWasm('test','root.usda',{signal:cancelled.signal,onProgress:()=>cancelled.abort()}),{name:'AbortError'});
 assert.equal(created,deleted);
 await assert.rejects(loader.streamFetchToWasm('test','root.usda',{onProgress:()=>{throw Error('callback failed');}}),/callback failed/);
 assert.equal(created,deleted);
 const external=new native.RenderStream();
 await assert.rejects(loader.streamFetchToWasm('test','root.usda',{usdInstance:external,onProgress:()=>{throw Error('stop');}}),/stop/);
 assert.deepEqual(external.getActiveZeroCopyBuffers(),[]);external.delete();
 globalThis.fetch=async()=>response(bytes.length);
 const phases=[];
 const loaded=await loader.loadWithStreaming('https://example.test/root.usda',{onProgress:x=>phases.push(x.stage)});
 assert.equal(loaded.numMeshes(),1);loaded.delete();assert.equal(phases.at(-1),'complete');
 const document=await loader.loadAsLayerAsync('https://example.test/root.usda');
 assert.ok(document.exportUSDA().text.includes('Box'));document.delete();
 const parsed=await loader.loadTestAsync('https://example.test/root.usda');
 assert.equal(parsed.numMeshes(),1);parsed.delete();
 assert.ok(await loader.getNativeDefaultMemoryLimitMB()>0);
 const full=await loader.loadWithFullProgressAsync('https://example.test/root.usda',()=>{},{});
 assert.equal(full.usd.numMeshes(),1);full.usd.delete();
 fs.writeFileSync(path.join(temp,'box.usda'),bytes);
 const file=await loader.streamFileToWasm(path.join(temp,'box.usda'),'file.usda',{chunkSize:3});
 assert.equal(file.usdInstance.beginStreamedAsset('file.usda').success,true);file.usdInstance.delete();
 const controller=new AbortController();controller.abort();
 await assert.rejects(loader.streamFileToWasm(path.join(temp,'box.usda'),'file.usda',{signal:controller.signal}),{name:'AbortError'});
 assert.equal(created,deleted);
 assert.deepEqual(await loader.getActiveStreamingBuffers(),[]);
 await assert.rejects(loader.streamFetchMultipleToWasm([],{concurrency:-1}),/concurrency/);
 console.log('PASS core stream transfer: known/unknown lengths, bounds, cancellation, ownership, file chunks and parse');
}finally{globalThis.fetch=fetchOriginal;native.RenderStream=Original;fs.rmSync(temp,{recursive:true,force:true});}
