// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
const suffix=process.env.LIGHTUSD_WASM64==='1'?'_64':'';
const {default:factory}=await import(`../src/lightusd/lightusd${suffix}.js`);
import {LightUSDLoader} from '../src/lightusd/LightUSDLoader.js';
const native=await factory();
assert.equal(native.LightUSDLoaderNative,undefined);
const Original=native.RenderStream;
let created=0,released=0;
native.RenderStream=function(...args) {
  const stream=new Original(...args);created++;
  let deleted=false;
  return new Proxy(stream,{get(target,key) {
    if(key==='delete')return ()=>{if(!deleted){deleted=true;released++;}return target.delete();};
    const value=Reflect.get(target,key);
    return typeof value==='function'?value.bind(target):value;
  }});
};
const loader=new LightUSDLoader(null,{maxMemoryLimitMB:64,backend:'legacy'});
assert.equal(loader.hasAsyncSupport(),false);
loader.native_=native;
assert.equal(loader.hasAsyncSupport(),true);
const bytes=new TextEncoder().encode('#usda 1.0\ndef Cube "Box" {}\n');
const previous=()=>{};
native.onNextCrateProgress=previous;
const phases=[];
const [first,second]=await Promise.all([
  loader.parseWithProgress(bytes,'box.usda',{onProgress:info=>phases.push(info.stage)}),
  loader.parseAsync(bytes,'box.usda',{onPhaseStart:info=>assert.equal(typeof info.progress,'number')})
]);
assert.equal(first.numMeshes(),1);assert.equal(second.numMeshes(),1);
assert.ok(phases.includes('native-load') && phases.includes('mesh-copy'));
assert.equal(phases.at(-1),'complete');
assert.equal(native.onNextCrateProgress,previous);
first.delete();second.delete();
const aborted=new AbortController();aborted.abort();
const before=created;
await assert.rejects(loader.parseWithProgress(bytes,'box.usda',{signal:aborted.signal}),{name:'AbortError'});
assert.equal(created,before);
for(const stage of ['archive','native-load','mesh-copy']) {
  const controller=new AbortController();
  await assert.rejects(loader.parseWithProgress(bytes,'box.usda',{
    signal:controller.signal,onProgress:info=>{if(info.stage===stage)controller.abort();}
  }),{name:'AbortError'});
  assert.equal(created,released);
  assert.equal(native.onNextCrateProgress,previous);
}
const timedAbort=new AbortController();
const pending=loader.parseWithProgress(bytes,'box.usda',{signal:timedAbort.signal});
setTimeout(()=>timedAbort.abort(),0);
await assert.rejects(pending,{name:'AbortError'});
assert.equal(created,released);
await assert.rejects(loader.parseWithProgress(bytes,'box.usda',{onProgress:info=>{
  if(info.stage==='native-load')throw Error('callback failed');
}}),/callback failed/);
await assert.rejects(loader.parseWithProgress(bytes,'invalid.usdz',{onProgress:()=>{}}));
assert.equal(native.onNextCrateProgress,previous);
loader.setMaxMemoryLimitMB(0.5);
await assert.rejects(loader.parseWithProgress(bytes,'box.usda'),/integer/);
assert.equal(created,released);
native.RenderStream=Original;
console.log('PASS: core loader progress, async phases, cancellation, limits, concurrency and cleanup');
